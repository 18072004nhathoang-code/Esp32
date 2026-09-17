#include "ai_voice_service.h"

#include "../audio/audio_manager.h"
#include "../os/wifi_manager.h"
#include "firmware_contracts.h"
#include "service_state_logic.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <ctype.h>
#include <limits.h>
#include <ArduinoJson.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef AI_VOICE_ENDPOINT
#define AI_VOICE_ENDPOINT ""
#endif
#ifndef AI_VOICE_TTS_ENDPOINT
#define AI_VOICE_TTS_ENDPOINT ""
#endif
#ifndef AI_VOICE_BEARER_TOKEN
#define AI_VOICE_BEARER_TOKEN ""
#endif
#ifndef AI_VOICE_CA_CERT
#define AI_VOICE_CA_CERT ""
#endif

namespace
{
volatile AIVoiceState s_state = AI_STATE_ERROR;
ChatMessage s_history[AI_MAX_CHAT_MESSAGES] = {};
int s_message_count = 0;
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;
bool s_recording_started = false;
uint32_t s_next_request_id = 0;
uint32_t s_pending_request_id = 0;
uint32_t s_active_request_id = 0;
uint32_t s_cancelled_through = 0;
uint32_t s_completed_request_id = 0;
uint32_t s_recording_base_generation = 0;
uint32_t s_recording_deadline_ms = 0;
bool s_waiting_record_start = false;
bool s_waiting_record_commit = false;
char s_last_error[128] = "AI endpoint is not configured";
static constexpr size_t kJsonBodyLimit = 16U * 1024U;
static constexpr size_t kTtsBodyLimit = 2U * 1024U * 1024U;
static constexpr uint32_t kRequestDeadlineMs = 45000;

bool request_cancelled(uint32_t request_id)
{
    if (!request_id || !s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return true;
    const bool cancelled = !request_response_is_current(
        request_id, s_active_request_id, s_cancelled_through);
    xSemaphoreGive(s_mutex);
    return cancelled;
}

bool deadline_expired(uint32_t deadline_ms)
{
    return static_cast<int32_t>(millis() - deadline_ms) >= 0;
}

void set_error(const char *message)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    strlcpy(s_last_error, message ? message : "Unknown AI error", sizeof(s_last_error));
    s_state = AI_STATE_ERROR;
    xSemaphoreGive(s_mutex);
}

const char *active_token()
{
    return AI_VOICE_BEARER_TOKEN;
}

bool configuration_ready()
{
    return strncmp(AI_VOICE_ENDPOINT, "https://", 8) == 0 &&
           strncmp(AI_VOICE_TTS_ENDPOINT, "https://", 8) == 0 &&
           active_token()[0] != '\0' && AI_VOICE_CA_CERT[0] != '\0';
}

void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
}

void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
    p[2] = (value >> 16) & 0xFF;
    p[3] = (value >> 24) & 0xFF;
}

class RecordedWavStream final : public Stream
{
public:
    RecordedWavStream(const AudioRecordingLease &lease, uint32_t request_id, uint32_t deadline_ms)
        : lease_(lease), request_id_(request_id), deadline_ms_(deadline_ms), position_(0)
    {
        const uint32_t data_bytes = lease_.sample_count * sizeof(int16_t);
        memset(header_, 0, sizeof(header_));
        memcpy(header_, "RIFF", 4); put_le32(header_ + 4, 36 + data_bytes);
        memcpy(header_ + 8, "WAVEfmt ", 8); put_le32(header_ + 16, 16);
        put_le16(header_ + 20, 1); put_le16(header_ + 22, 1);
        put_le32(header_ + 24, AUDIO_SAMPLE_RATE);
        put_le32(header_ + 28, AUDIO_SAMPLE_RATE * sizeof(int16_t));
        put_le16(header_ + 32, sizeof(int16_t)); put_le16(header_ + 34, 16);
        memcpy(header_ + 36, "data", 4); put_le32(header_ + 40, data_bytes);
    }

    size_t total_size() const { return sizeof(header_) + lease_.sample_count * sizeof(int16_t); }
    int available() override
    {
        const size_t remaining = total_size() - position_;
        return remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
    }
    int read() override
    {
        if (position_ >= total_size() || request_cancelled(request_id_) || deadline_expired(deadline_ms_)) return -1;
        if (position_ < sizeof(header_)) return header_[position_++];
        const size_t byte_offset = position_++ - sizeof(header_);
        int16_t sample = 0;
        if (audio_copy_recording_lease(&lease_, byte_offset / 2, &sample, 1) != 1) return -1;
        return (byte_offset & 1U) ? (((uint16_t)sample >> 8) & 0xFF) : ((uint16_t)sample & 0xFF);
    }
    size_t readBytes(char *buffer, size_t length) override
    {
        if (!buffer || length == 0 || position_ >= total_size() ||
            request_cancelled(request_id_) || deadline_expired(deadline_ms_)) return 0;
        size_t remaining = total_size() - position_;
        if (length > remaining) length = remaining;
        size_t written = 0;
        if (position_ < sizeof(header_))
        {
            size_t bytes = sizeof(header_) - position_;
            if (bytes > length) bytes = length;
            memcpy(buffer, header_ + position_, bytes);
            position_ += bytes;
            buffer += bytes;
            length -= bytes;
            written += bytes;
        }
        while (length > 0)
        {
            const size_t byte_offset = position_ - sizeof(header_);
            if ((byte_offset & 1U) != 0 || length == 1)
            {
                int value = read();
                if (value < 0) break;
                *buffer++ = static_cast<char>(value);
                --length;
                ++written;
                continue;
            }
            size_t samples = length / sizeof(int16_t);
            if (samples > 256) samples = 256;
            int16_t chunk[256];
            const size_t got = audio_copy_recording_lease(&lease_, byte_offset / 2, chunk, samples);
            if (got == 0) break;
            const size_t bytes = got * sizeof(int16_t);
            memcpy(buffer, chunk, bytes);
            buffer += bytes;
            length -= bytes;
            written += bytes;
            position_ += bytes;
        }
        return written;
    }
    int peek() override
    {
        const size_t saved = position_;
        const int value = read();
        position_ = saved;
        return value;
    }
    void flush() override {}
    size_t write(uint8_t) override { return 0; }

private:
    AudioRecordingLease lease_;
    uint32_t request_id_;
    uint32_t deadline_ms_;
    size_t position_;
    uint8_t header_[44];
};

class BoundedBodyStream final : public Stream
{
public:
    BoundedBodyStream(size_t limit, uint32_t request_id, uint32_t deadline_ms)
        : data_(nullptr), size_(0), limit_(limit), request_id_(request_id),
          deadline_ms_(deadline_ms), failed_(false)
    {
        data_ = static_cast<uint8_t *>(heap_caps_malloc(limit_ + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!data_) data_ = static_cast<uint8_t *>(malloc(limit_ + 1));
        if (!data_) failed_ = true;
    }
    ~BoundedBodyStream() override { if (data_) free(data_); }
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t *buffer, size_t length) override
    {
        if (!buffer || failed_ || request_cancelled(request_id_) || deadline_expired(deadline_ms_) ||
            !bounded_body_append_allowed(size_, length, limit_))
        {
            failed_ = true;
            return 0;
        }
        memcpy(data_ + size_, buffer, length);
        size_ += length;
        data_[size_] = 0;
        return length;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    const uint8_t *data() const { return data_; }
    size_t size() const { return size_; }
    bool failed() const { return failed_; }
private:
    uint8_t *data_;
    size_t size_;
    size_t limit_;
    uint32_t request_id_;
    uint32_t deadline_ms_;
    bool failed_;
};

bool parse_ai_response_json(const uint8_t *data, size_t size, char *transcript,
                            size_t transcript_size, char *reply, size_t reply_size)
{
    if (!data || size == 0 || !transcript || transcript_size < 2 || !reply || reply_size < 2)
        return false;
    size_t first = 0;
    while (first < size && isspace(data[first])) ++first;
    if (first == size || data[first] != '{') return false;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    size_t document_end = 0;
    for (size_t i = first; i < size; ++i)
    {
        const uint8_t c = data[i];
        if (in_string)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']')
        {
            if (--depth < 0) return false;
            if (depth == 0) { document_end = i + 1; break; }
        }
    }
    if (document_end == 0 || in_string || escaped) return false;
    for (size_t i = document_end; i < size; ++i)
        if (!isspace(data[i])) return false;

    DynamicJsonDocument document(4096);
    const DeserializationError error = deserializeJson(
        document, data, size, DeserializationOption::NestingLimit(4));
    if (error || !document.is<JsonObject>()) return false;
    JsonObject root = document.as<JsonObject>();
    if (root.size() != 2 || !root.containsKey("transcript") || !root.containsKey("reply") ||
        !root["transcript"].is<const char *>() || !root["reply"].is<const char *>()) return false;
    const char *transcript_value = root["transcript"].as<const char *>();
    const char *reply_value = root["reply"].as<const char *>();
    const size_t transcript_length = transcript_value ? strlen(transcript_value) : 0;
    const size_t reply_length = reply_value ? strlen(reply_value) : 0;
    if (transcript_length == 0 || reply_length == 0 || transcript_length >= transcript_size ||
        reply_length >= reply_size || !valid_utf8_text(transcript_value, transcript_length) ||
        !valid_utf8_text(reply_value, reply_length)) return false;
    memcpy(transcript, transcript_value, transcript_length + 1);
    memcpy(reply, reply_value, reply_length + 1);
    return true;
}

bool serialize_tts_request_json(const char *text, String &body)
{
    if (!text || !*text || !valid_utf8_text(text, strlen(text))) return false;
    StaticJsonDocument<768> request_json;
    request_json["text"] = text;
    body = "";
    return serializeJson(request_json, body) > 0;
}

bool post_recording(uint32_t request_id, char *transcript, size_t transcript_size,
                    char *reply, size_t reply_size)
{
    if (!wifi_manager_is_connected()) { set_error("WiFi is not connected"); return false; }
    AudioRecordingLease lease = {};
    if (!audio_acquire_recording_lease(&lease) || lease.sample_count < AUDIO_SAMPLE_RATE / 4)
    {
        audio_release_recording_lease(&lease);
        set_error("Recording is too short");
        return false;
    }
    const uint32_t deadline_ms = millis() + kRequestDeadlineMs;

    WiFiClientSecure tls;
    tls.setCACert(AI_VOICE_CA_CERT);
    HTTPClient http;
    if (!http.begin(tls, AI_VOICE_ENDPOINT))
    {
        audio_release_recording_lease(&lease);
        set_error("Cannot open AI HTTPS endpoint");
        return false;
    }
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("Accept", "application/json");
    http.addHeader("Authorization", String("Bearer ") + active_token());

    RecordedWavStream wav(lease, request_id, deadline_ms);
    const int code = http.sendRequest("POST", &wav, wav.total_size());
    audio_release_recording_lease(&lease);
    if (code < 200 || code >= 300)
    {
        http.end();
        set_error(code > 0 ? "AI endpoint rejected the request" : "AI HTTPS request failed");
        return false;
    }
    const int response_size = http.getSize();
    if (response_size > static_cast<int>(kJsonBodyLimit) || request_cancelled(request_id))
    {
        http.end();
        set_error("AI response exceeds limit");
        return false;
    }
    BoundedBodyStream sink(kJsonBodyLimit, request_id, deadline_ms);
    const int written = http.writeToStream(&sink); // HTTPClient removes chunk framing here.
    http.end();
    if (!http_dechunked_body_complete(response_size, sink.size(), written, sink.failed()) ||
        request_cancelled(request_id))
    {
        set_error("AI response is truncated, cancelled or oversized");
        return false;
    }
    if (!parse_ai_response_json(sink.data(), sink.size(), transcript, transcript_size,
                                reply, reply_size))
    {
        set_error("AI response JSON is invalid");
        return false;
    }
    return true;
}

bool stream_tts_wav(uint32_t request_id, const char *text)
{
    if (!text || !*text || !wifi_manager_is_connected()) return false;
    const uint32_t deadline_ms = millis() + kRequestDeadlineMs;
    WiFiClientSecure tls;
    tls.setCACert(AI_VOICE_CA_CERT);
    HTTPClient http;
    if (!http.begin(tls, AI_VOICE_TTS_ENDPOINT)) return false;
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "audio/wav");
    http.addHeader("Authorization", String("Bearer ") + active_token());
    String body;
    if (!serialize_tts_request_json(text, body)) { http.end(); return false; }
    int code = http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(body.c_str())), body.length());
    const int content_length = http.getSize();
    if (code < 200 || code >= 300 ||
        (content_length >= 0 && content_length > static_cast<int>(kTtsBodyLimit)) ||
        request_cancelled(request_id))
    {
        http.end();
        return false;
    }

    BoundedBodyStream sink(kTtsBodyLimit, request_id, deadline_ms);
    const int written = http.writeToStream(&sink); // Dechunk before RIFF parsing.
    http.end();
    if (!http_dechunked_body_complete(content_length, sink.size(), written, sink.failed()) ||
        request_cancelled(request_id))
    {
        return false;
    }
    PcmWavView wav = {};
    if (!parse_pcm16_mono_16k_wav(sink.data(), sink.size(), &wav) ||
        !audio_request_ownership(AUDIO_OWNER_AI_VOICE))
    {
        return false;
    }

    const uint32_t audio_session = audio_get_owner_session(AUDIO_OWNER_AI_VOICE);
    bool ok = audio_codec_configure_for_stream(AUDIO_SAMPLE_RATE, 256);
    ok = ok && audio_session != 0 &&
         audio_set_pa_for_session(AUDIO_OWNER_AI_VOICE, audio_session, true);
    size_t offset = 0;
    while (ok && offset < wav.sample_count)
    {
        if (request_cancelled(request_id) || deadline_expired(deadline_ms)) { ok = false; break; }
        size_t count = wav.sample_count - offset;
        if (count > 256) count = 256;
        if (!audio_write_pcm16_mono(wav.samples + offset, count, 250)) { ok = false; break; }
        offset += count;
    }
    if (ok) ok = audio_drain_tx(400);
    if (audio_session) audio_release_ownership_session(AUDIO_OWNER_AI_VOICE, audio_session);
    else audio_release_ownership(AUDIO_OWNER_AI_VOICE);
    return ok;
}

void ai_task(void *)
{
    for (;;)
    {
        uint32_t request_id = 0;
        bool wait_start = false;
        bool wait_commit = false;
        uint32_t base_generation = 0;
        uint32_t recording_deadline = 0;
        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            wait_start = s_waiting_record_start;
            wait_commit = s_waiting_record_commit;
            base_generation = s_recording_base_generation;
            recording_deadline = s_recording_deadline_ms;
            if (s_pending_request_id != 0)
            {
                request_id = s_pending_request_id;
                s_pending_request_id = 0;
                s_active_request_id = request_id;
                s_state = AI_STATE_PROCESSING;
            }
            xSemaphoreGive(s_mutex);
        }
        if (wait_start && (audio_is_recording() || deadline_expired(recording_deadline)))
        {
            const bool started = audio_is_recording();
            bool cancel_stale_start = false;
            if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
            {
                s_waiting_record_start = false;
                if (!started && s_state == AI_STATE_LISTENING)
                {
                    s_recording_started = false;
                    cancel_stale_start = true;
                    strlcpy(s_last_error, "Microphone/I2S start failed", sizeof(s_last_error));
                    s_state = AI_STATE_ERROR;
                }
                xSemaphoreGive(s_mutex);
            }
            // Invalidates the queued audio generation as well as stopping a
            // start that raced the timeout. It cannot retain RECORDER I2S.
            if (cancel_stale_start && !audio_cancel_recording_async())
                Serial.println("[AI] Unable to invalidate timed-out recording start");
        }
        if (wait_commit)
        {
            const uint32_t generation = audio_get_recording_generation();
            const bool committed = generation != base_generation;
            if (committed || deadline_expired(recording_deadline))
            {
                bool cancel_failed_commit = false;
                if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
                {
                    s_waiting_record_commit = false;
                    if (committed && s_state == AI_STATE_PROCESSING &&
                        !s_pending_request_id && !s_active_request_id)
                    {
                        s_pending_request_id = ++s_next_request_id;
                    }
                    else if (!committed && s_state == AI_STATE_PROCESSING)
                    {
                        cancel_failed_commit = true;
                        strlcpy(s_last_error, "Recording finalize failed", sizeof(s_last_error));
                        s_state = AI_STATE_ERROR;
                    }
                    xSemaphoreGive(s_mutex);
                }
                if (cancel_failed_commit && !audio_cancel_recording_async())
                    Serial.println("[AI] Unable to cancel failed recording finalize");
            }
        }
        if (request_id != 0)
        {
            char transcript[AI_MAX_TEXT_LEN] = {};
            char reply[AI_MAX_TEXT_LEN] = {};
            const bool response_ok = post_recording(
                request_id, transcript, sizeof(transcript), reply, sizeof(reply));
            if (response_ok && !request_cancelled(request_id))
            {
                ai_voice_add_message(true, transcript);
                ai_voice_add_message(false, reply);
                if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                {
                    if (request_response_is_current(request_id, s_active_request_id, s_cancelled_through))
                        s_state = AI_STATE_SPEAKING;
                    xSemaphoreGive(s_mutex);
                }
                if (!request_cancelled(request_id) && !stream_tts_wav(request_id, reply) &&
                    !request_cancelled(request_id))
                    set_error("TTS HTTPS/WAV playback failed");
            }
            if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
            {
                if (ai_cleanup_must_clear(request_id, s_active_request_id))
                    s_active_request_id = 0;
                s_completed_request_id = request_id;
                if (s_state == AI_STATE_CANCELING || request_id <= s_cancelled_through)
                    s_state = ai_voice_is_available() ? AI_STATE_IDLE : AI_STATE_ERROR;
                else if (s_state != AI_STATE_ERROR)
                    s_state = AI_STATE_IDLE;
                xSemaphoreGive(s_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}

bool ai_voice_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) { set_error("Cannot create AI mutex"); return false; }
    ai_voice_clear_history();
    if (!configuration_ready())
    {
        set_error("Configure HTTPS AI/TTS endpoints, CA and token in secrets.h");
        ai_voice_add_message(false, s_last_error);
        return false;
    }
    if (!s_task)
    {
        BaseType_t created = xTaskCreatePinnedToCore(ai_task, "AIVoiceTask", 12288,
                                                     nullptr, 2, &s_task, 0);
        if (created != pdPASS) { s_task = nullptr; set_error("Cannot create AI task"); return false; }
    }
    s_state = AI_STATE_IDLE;
    ai_voice_add_message(false, "AI Voice HTTPS service ready.");
    return true;
}

bool ai_voice_is_available(void) { return s_task != nullptr && configuration_ready(); }
const char *ai_voice_get_last_error(void) { return s_last_error; }

bool ai_voice_start_recording(void)
{
    if (!ai_voice_is_available()) { set_error("AI Voice is not configured"); return false; }
    if (!wifi_manager_is_connected()) { set_error("WiFi is not connected"); return false; }
    const uint32_t base_generation = audio_get_recording_generation();
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (s_state != AI_STATE_IDLE || s_recording_started || s_pending_request_id || s_active_request_id)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_recording_started = true;
    s_recording_base_generation = base_generation;
    s_recording_deadline_ms = millis() + 1000;
    s_waiting_record_start = true;
    s_waiting_record_commit = false;
    s_state = AI_STATE_LISTENING;
    xSemaphoreGive(s_mutex);
    if (!audio_start_recording_async(AUDIO_RECORD_MAX_SEC))
    {
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_recording_started = false;
            s_waiting_record_start = false;
            strlcpy(s_last_error, "Audio command queue is busy", sizeof(s_last_error));
            s_state = AI_STATE_ERROR;
            xSemaphoreGive(s_mutex);
        }
        return false;
    }
    return true;
}

bool ai_voice_stop_and_process(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (s_state != AI_STATE_LISTENING || !s_recording_started)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_recording_started = false;
    s_waiting_record_start = false;
    s_waiting_record_commit = true;
    s_recording_deadline_ms = millis() + 2000;
    s_state = AI_STATE_PROCESSING;
    xSemaphoreGive(s_mutex);
    if (!audio_stop_recording_async())
    {
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_waiting_record_commit = false;
            strlcpy(s_last_error, "Audio command queue is busy", sizeof(s_last_error));
            s_state = AI_STATE_ERROR;
            xSemaphoreGive(s_mutex);
        }
        return false;
    }
    return true;
}

void ai_voice_cancel(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const bool cancel_recording = s_recording_started;
    s_recording_started = false;
    s_waiting_record_start = false;
    s_waiting_record_commit = false;
    s_state = AI_STATE_CANCELING;
    if (s_pending_request_id)
    {
        if (s_pending_request_id > s_cancelled_through) s_cancelled_through = s_pending_request_id;
        s_completed_request_id = s_pending_request_id;
        s_pending_request_id = 0;
    }
    if (s_active_request_id > s_cancelled_through) s_cancelled_through = s_active_request_id;
    const bool worker_active = s_active_request_id != 0;
    xSemaphoreGive(s_mutex);
    if (cancel_recording && !audio_cancel_recording_async())
        Serial.println("[AI] Unable to enqueue recording cancel");
    if (!worker_active && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_state = ai_voice_is_available() ? AI_STATE_IDLE : AI_STATE_ERROR;
        xSemaphoreGive(s_mutex);
    }
}

AIVoiceState ai_voice_get_state(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return AI_STATE_ERROR;
    const AIVoiceState state = s_state;
    xSemaphoreGive(s_mutex);
    return state;
}

const char *ai_voice_get_state_text(void)
{
    switch (ai_voice_get_state())
    {
        case AI_STATE_LISTENING: return "Đang thu âm từ microphone...";
        case AI_STATE_PROCESSING: return "Đang gửi HTTPS và xử lý...";
        case AI_STATE_SPEAKING: return "Đang phát TTS WAV...";
        case AI_STATE_CANCELING: return "Đang hủy và chờ worker dừng...";
        case AI_STATE_ERROR: return s_last_error;
        default: return "Giữ nút để nói";
    }
}

int ai_voice_get_message_count(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    const int count = s_message_count;
    xSemaphoreGive(s_mutex);
    return count;
}

bool ai_voice_get_message_copy(int index, ChatMessage *out_msg)
{
    if (!out_msg || index < 0 || !s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    const bool ok = index < s_message_count;
    if (ok) *out_msg = s_history[index];
    xSemaphoreGive(s_mutex);
    return ok;
}

void ai_voice_add_message(bool is_user, const char *text)
{
    if (!text || !*text || !s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (s_message_count == AI_MAX_CHAT_MESSAGES)
    {
        memmove(s_history, s_history + 1, sizeof(ChatMessage) * (AI_MAX_CHAT_MESSAGES - 1));
        --s_message_count;
    }
    ChatMessage &message = s_history[s_message_count++];
    message.is_user = is_user;
    strlcpy(message.text, text, sizeof(message.text));
    message.timestamp_sec = millis() / 1000;
    xSemaphoreGive(s_mutex);
}

void ai_voice_clear_history(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    memset(s_history, 0, sizeof(s_history));
    s_message_count = 0;
    xSemaphoreGive(s_mutex);
}

bool ai_voice_play_tts(const char *text)
{
    if (!configuration_ready() || !s_mutex ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (s_state != AI_STATE_IDLE || s_active_request_id || s_pending_request_id)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    const uint32_t request_id = ++s_next_request_id;
    s_active_request_id = request_id;
    s_state = AI_STATE_SPEAKING;
    xSemaphoreGive(s_mutex);
    const bool ok = stream_tts_wav(request_id, text);
    bool cancelled = false;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        const bool own_request = ai_cleanup_must_clear(request_id, s_active_request_id);
        if (own_request)
        {
            s_active_request_id = 0;
            cancelled = request_id <= s_cancelled_through;
            if (cancelled || ok)
                s_state = AI_STATE_IDLE;
            else
            {
                strlcpy(s_last_error, "TTS HTTPS/WAV playback failed", sizeof(s_last_error));
                s_state = AI_STATE_ERROR;
            }
        }
        s_completed_request_id = request_id;
        xSemaphoreGive(s_mutex);
    }
    return ok;
}

bool ai_voice_json_regression_test(void)
{
    char transcript[64] = {};
    char reply[64] = {};
    const char valid[] = "{\"transcript\":\"xin ch\\u00e0o\",\"reply\":\"d\\u00f2ng 1\\n\\t2\"}";
    const char trailing[] = "{\"transcript\":\"a\",\"reply\":\"b\"} garbage";
    const char wrong_type[] = "{\"transcript\":1,\"reply\":\"b\"}";
    if (!parse_ai_response_json(reinterpret_cast<const uint8_t *>(valid), strlen(valid),
                                transcript, sizeof(transcript), reply, sizeof(reply)) ||
        parse_ai_response_json(reinterpret_cast<const uint8_t *>(trailing), strlen(trailing),
                               transcript, sizeof(transcript), reply, sizeof(reply)) ||
        parse_ai_response_json(reinterpret_cast<const uint8_t *>(wrong_type), strlen(wrong_type),
                               transcript, sizeof(transcript), reply, sizeof(reply))) return false;
    String encoded;
    if (!serialize_tts_request_json("tab\tnewline\nquote\"", encoded)) return false;
    StaticJsonDocument<128> decoded;
    if (deserializeJson(decoded, encoded) || !decoded["text"].is<const char *>()) return false;
    return strcmp(decoded["text"].as<const char *>(), "tab\tnewline\nquote\"") == 0;
}
