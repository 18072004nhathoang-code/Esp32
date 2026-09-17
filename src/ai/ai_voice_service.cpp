#include "ai_voice_service.h"

#include "../audio/audio_manager.h"
#include "../os/wifi_manager.h"
#include "firmware_contracts.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <ctype.h>
#include <limits.h>

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
    const bool locked = s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
    strlcpy(s_last_error, message ? message : "Unknown AI error", sizeof(s_last_error));
    s_state = AI_STATE_ERROR;
    if (locked) xSemaphoreGive(s_mutex);
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

bool extract_json_string(const String &json, const char *key, char *out, size_t out_size)
{
    if (!key || !out || out_size == 0) return false;
    int pos = -1;
    int depth = 0;
    const size_t key_len = strlen(key);
    for (int i = 0; i < static_cast<int>(json.length()); ++i)
    {
        const char c = json[i];
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') { --depth; if (depth < 0) return false; continue; }
        if (c != '"') continue;
        const int token_start = ++i;
        bool token_escape = false;
        while (i < static_cast<int>(json.length()))
        {
            const char value = json[i];
            if (token_escape) token_escape = false;
            else if (value == '\\') token_escape = true;
            else if (value == '"') break;
            ++i;
        }
        if (i >= static_cast<int>(json.length())) return false;
        if (depth != 1 || token_escape || static_cast<size_t>(i - token_start) != key_len ||
            strncmp(json.c_str() + token_start, key, key_len) != 0) continue;
        int cursor = i + 1;
        while (cursor < static_cast<int>(json.length()) &&
               isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
        if (cursor >= static_cast<int>(json.length()) || json[cursor++] != ':') return false;
        while (cursor < static_cast<int>(json.length()) &&
               isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
        if (cursor >= static_cast<int>(json.length()) || json[cursor] != '"') return false;
        pos = cursor + 1;
        break;
    }
    if (pos < 0 || depth < 0) return false;
    size_t written = 0;
    bool escaped = false;
    bool closed = false;
    bool overflow = false;
    auto hex_value = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    auto append_utf8 = [&](uint32_t cp) {
        uint8_t bytes[4];
        size_t count = 0;
        if (cp <= 0x7F) bytes[count++] = cp;
        else if (cp <= 0x7FF)
        {
            bytes[count++] = 0xC0 | (cp >> 6);
            bytes[count++] = 0x80 | (cp & 0x3F);
        }
        else if (cp <= 0xFFFF)
        {
            bytes[count++] = 0xE0 | (cp >> 12);
            bytes[count++] = 0x80 | ((cp >> 6) & 0x3F);
            bytes[count++] = 0x80 | (cp & 0x3F);
        }
        else if (cp <= 0x10FFFF)
        {
            bytes[count++] = 0xF0 | (cp >> 18);
            bytes[count++] = 0x80 | ((cp >> 12) & 0x3F);
            bytes[count++] = 0x80 | ((cp >> 6) & 0x3F);
            bytes[count++] = 0x80 | (cp & 0x3F);
        }
        if (written + count < out_size)
            for (size_t i = 0; i < count; ++i) out[written++] = static_cast<char>(bytes[i]);
        else overflow = true;
    };
    for (; pos < (int)json.length(); ++pos)
    {
        char c = json[pos];
        if (escaped)
        {
            if (c == 'u' && pos + 4 < (int)json.length())
            {
                uint32_t cp = 0;
                bool valid_hex = true;
                for (int i = 1; i <= 4; ++i)
                {
                    int nibble = hex_value(json[pos + i]);
                    if (nibble < 0) { valid_hex = false; break; }
                    cp = (cp << 4) | static_cast<uint32_t>(nibble);
                }
                if (valid_hex)
                {
                    pos += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF)
                    {
                        if (pos + 6 >= (int)json.length() || json[pos + 1] != '\\' ||
                            json[pos + 2] != 'u') return false;
                        uint32_t low = 0;
                        bool valid_low = true;
                        for (int i = 3; i <= 6; ++i)
                        {
                            int nibble = hex_value(json[pos + i]);
                            if (nibble < 0) { valid_low = false; break; }
                            low = (low << 4) | static_cast<uint32_t>(nibble);
                        }
                        if (valid_low && low >= 0xDC00 && low <= 0xDFFF)
                        {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            pos += 6;
                        }
                        else return false;
                    }
                    else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                    append_utf8(cp);
                }
                else return false;
            }
            else
            {
                if (c == 'n') c = '\n';
                else if (c == 'r') c = '\r';
                else if (c == 't') c = '\t';
                else if (c != '"' && c != '\\' && c != '/' && c != 'b' && c != 'f') return false;
                if (c == 'b') c = '\b';
                if (c == 'f') c = '\f';
                if (written + 1 < out_size) out[written++] = c;
                else overflow = true;
            }
            escaped = false;
        }
        else if (c == '\\') escaped = true;
        else if (c == '"') { closed = true; break; }
        else if (static_cast<uint8_t>(c) < 0x20) return false;
        else if (written + 1 < out_size) out[written++] = c;
        else overflow = true;
    }
    out[written] = '\0';
    return closed && !escaped && !overflow && written > 0;
}

bool json_object_envelope_valid(const String &json)
{
    size_t begin = 0;
    while (begin < json.length() && isspace(static_cast<unsigned char>(json[begin]))) ++begin;
    size_t end = json.length();
    while (end > begin && isspace(static_cast<unsigned char>(json[end - 1]))) --end;
    if (end <= begin + 1 || json[begin] != '{' || json[end - 1] != '}') return false;
    char stack[16] = {};
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = begin; i < end; ++i)
    {
        const char c = json[i];
        if (in_string)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            else if (static_cast<uint8_t>(c) < 0x20) return false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{' || c == '[')
        {
            if (depth >= sizeof(stack)) return false;
            stack[depth++] = c;
        }
        else if (c == '}' || c == ']')
        {
            if (depth == 0) return false;
            const char open = stack[--depth];
            if ((c == '}' && open != '{') || (c == ']' && open != '[')) return false;
        }
    }
    return depth == 0 && !in_string && !escaped;
}

String json_escape(const char *text)
{
    String escaped;
    if (!text) return escaped;
    escaped.reserve(strlen(text) + 16);
    for (const char *p = text; *p; ++p)
    {
        if (*p == '"' || *p == '\\') escaped += '\\';
        if (*p == '\n') escaped += "\\n";
        else if (*p != '\r') escaped += *p;
    }
    return escaped;
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
    const String response(reinterpret_cast<const char *>(sink.data()));
    if (!json_object_envelope_valid(response) ||
        !extract_json_string(response, "transcript", transcript, transcript_size) ||
        !extract_json_string(response, "reply", reply, reply_size))
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
    String body = String("{\"text\":\"") + json_escape(text) + "\"}";
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

    bool ok = true;
    size_t offset = 0;
    while (offset < wav.sample_count)
    {
        if (request_cancelled(request_id) || deadline_expired(deadline_ms)) { ok = false; break; }
        size_t count = wav.sample_count - offset;
        if (count > 256) count = 256;
        if (!audio_write_pcm16_mono(wav.samples + offset, count, 250)) { ok = false; break; }
        offset += count;
    }
    audio_release_ownership(AUDIO_OWNER_AI_VOICE);
    return ok;
}

void ai_task(void *)
{
    for (;;)
    {
        uint32_t request_id = 0;
        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            if (s_pending_request_id != 0)
            {
                request_id = s_pending_request_id;
                s_pending_request_id = 0;
                s_active_request_id = request_id;
                s_state = AI_STATE_PROCESSING;
            }
            xSemaphoreGive(s_mutex);
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
            if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                if (s_active_request_id == request_id) s_active_request_id = 0;
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
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (s_state != AI_STATE_IDLE || s_recording_started || s_pending_request_id || s_active_request_id)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    s_recording_started = true;
    s_state = AI_STATE_LISTENING;
    xSemaphoreGive(s_mutex);
    if (!audio_start_recording(AUDIO_RECORD_MAX_SEC))
    {
        bool report_error = true;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            report_error = s_state == AI_STATE_LISTENING;
            s_recording_started = false;
            xSemaphoreGive(s_mutex);
        }
        if (report_error) set_error("Microphone/I2S is busy");
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        const bool still_listening = s_recording_started && s_state == AI_STATE_LISTENING;
        xSemaphoreGive(s_mutex);
        if (!still_listening)
        {
            audio_cancel_recording();
            return false;
        }
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
    s_state = AI_STATE_PROCESSING;
    xSemaphoreGive(s_mutex);
    audio_stop_recording();
    if (audio_get_recorded_sample_count() == 0)
    {
        bool still_processing = false;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            still_processing = s_state == AI_STATE_PROCESSING;
            xSemaphoreGive(s_mutex);
        }
        if (still_processing) set_error("No audio was recorded");
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (s_state != AI_STATE_PROCESSING || s_pending_request_id || s_active_request_id)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    const uint32_t request_id = ++s_next_request_id;
    s_pending_request_id = request_id;
    xSemaphoreGive(s_mutex);
    return true;
}

void ai_voice_cancel(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const bool cancel_recording = s_recording_started;
    s_recording_started = false;
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
    if (cancel_recording) audio_cancel_recording();
    if (!worker_active && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_state = ai_voice_is_available() ? AI_STATE_IDLE : AI_STATE_ERROR;
        xSemaphoreGive(s_mutex);
    }
}

AIVoiceState ai_voice_get_state(void) { return s_state; }

const char *ai_voice_get_state_text(void)
{
    switch (s_state)
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
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (s_active_request_id == request_id) s_active_request_id = 0;
        s_completed_request_id = request_id;
        cancelled = request_id <= s_cancelled_through;
        if (cancelled) s_state = AI_STATE_IDLE;
        else if (ok) s_state = AI_STATE_IDLE;
        xSemaphoreGive(s_mutex);
    }
    if (!ok && !cancelled) set_error("TTS HTTPS/WAV playback failed");
    return ok;
}
