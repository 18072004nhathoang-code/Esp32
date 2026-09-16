#include "ai_voice_service.h"

#include "../audio/audio_manager.h"
#include "../os/wifi_manager.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
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
volatile bool s_process_requested = false;
bool s_recording_started = false;
char s_last_error[128] = "AI endpoint is not configured";

void set_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "Unknown AI error", sizeof(s_last_error));
    s_state = AI_STATE_ERROR;
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
    explicit RecordedWavStream(size_t samples) : samples_(samples), position_(0)
    {
        const uint32_t data_bytes = samples_ * sizeof(int16_t);
        memset(header_, 0, sizeof(header_));
        memcpy(header_, "RIFF", 4); put_le32(header_ + 4, 36 + data_bytes);
        memcpy(header_ + 8, "WAVEfmt ", 8); put_le32(header_ + 16, 16);
        put_le16(header_ + 20, 1); put_le16(header_ + 22, 1);
        put_le32(header_ + 24, AUDIO_SAMPLE_RATE);
        put_le32(header_ + 28, AUDIO_SAMPLE_RATE * sizeof(int16_t));
        put_le16(header_ + 32, sizeof(int16_t)); put_le16(header_ + 34, 16);
        memcpy(header_ + 36, "data", 4); put_le32(header_ + 40, data_bytes);
    }

    size_t total_size() const { return sizeof(header_) + samples_ * sizeof(int16_t); }
    int available() override
    {
        const size_t remaining = total_size() - position_;
        return remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
    }
    int read() override
    {
        if (position_ >= total_size()) return -1;
        if (position_ < sizeof(header_)) return header_[position_++];
        const size_t byte_offset = position_++ - sizeof(header_);
        int16_t sample = 0;
        if (audio_copy_recorded_samples(byte_offset / 2, &sample, 1) != 1) return -1;
        return (byte_offset & 1U) ? (((uint16_t)sample >> 8) & 0xFF) : ((uint16_t)sample & 0xFF);
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
    size_t samples_;
    size_t position_;
    uint8_t header_[44];
};

bool extract_json_string(const String &json, const char *key, char *out, size_t out_size)
{
    if (!key || !out || out_size == 0) return false;
    String marker = String("\"") + key + "\"";
    int pos = json.indexOf(marker);
    if (pos < 0) return false;
    pos = json.indexOf(':', pos + marker.length());
    if (pos < 0) return false;
    pos = json.indexOf('"', pos + 1);
    if (pos < 0) return false;
    ++pos;
    size_t written = 0;
    bool escaped = false;
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
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 < (int)json.length() &&
                        json[pos + 1] == '\\' && json[pos + 2] == 'u')
                    {
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
                    }
                    append_utf8(cp);
                }
            }
            else
            {
                if (c == 'n') c = '\n';
                else if (c == 'r') c = '\r';
                else if (c == 't') c = '\t';
                if (written + 1 < out_size) out[written++] = c;
            }
            escaped = false;
        }
        else if (c == '\\') escaped = true;
        else if (c == '"') break;
        else if (written + 1 < out_size) out[written++] = c;
    }
    out[written] = '\0';
    return written > 0;
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

bool post_recording(char *transcript, size_t transcript_size, char *reply, size_t reply_size)
{
    if (!wifi_manager_is_connected()) { set_error("WiFi is not connected"); return false; }
    const size_t sample_count = audio_get_recorded_sample_count();
    if (sample_count < AUDIO_SAMPLE_RATE / 4) { set_error("Recording is too short"); return false; }

    WiFiClientSecure tls;
    tls.setCACert(AI_VOICE_CA_CERT);
    HTTPClient http;
    if (!http.begin(tls, AI_VOICE_ENDPOINT)) { set_error("Cannot open AI HTTPS endpoint"); return false; }
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("Accept", "application/json");
    http.addHeader("Authorization", String("Bearer ") + active_token());

    RecordedWavStream wav(sample_count);
    const int code = http.sendRequest("POST", &wav, wav.total_size());
    if (code < 200 || code >= 300)
    {
        http.end();
        set_error(code > 0 ? "AI endpoint rejected the request" : "AI HTTPS request failed");
        return false;
    }
    const int response_size = http.getSize();
    if (response_size > 16384)
    {
        http.end();
        set_error("AI response exceeds limit");
        return false;
    }
    const String response = http.getString();
    http.end();
    if (!extract_json_string(response, "transcript", transcript, transcript_size) ||
        !extract_json_string(response, "reply", reply, reply_size))
    {
        set_error("AI response JSON is invalid");
        return false;
    }
    return true;
}

bool stream_tts_wav(const char *text)
{
    if (!text || !*text || !wifi_manager_is_connected()) return false;
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
    if (code < 200 || code >= 300 || (content_length >= 0 && content_length > 2 * 1024 * 1024))
    {
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t header[44];
    if (!stream || stream->readBytes(reinterpret_cast<char *>(header), sizeof(header)) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0 ||
        header[20] != 1 || header[22] != 1 || header[34] != 16)
    {
        http.end();
        return false;
    }
    const uint32_t sample_rate = (uint32_t)header[24] | ((uint32_t)header[25] << 8) |
                                 ((uint32_t)header[26] << 16) | ((uint32_t)header[27] << 24);
    if (sample_rate != AUDIO_SAMPLE_RATE || !audio_request_ownership(AUDIO_OWNER_AI_VOICE))
    {
        http.end();
        return false;
    }

    bool ok = true;
    int16_t pcm[256];
    size_t received = sizeof(header);
    uint32_t last_data_ms = millis();
    while ((http.connected() || stream->available() > 0) &&
           (content_length < 0 || received < (size_t)content_length))
    {
        int available = stream->available();
        if (available <= 0)
        {
            if (millis() - last_data_ms > 10000) { ok = false; break; }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t bytes = static_cast<size_t>(available);
        if (bytes > sizeof(pcm)) bytes = sizeof(pcm);
        bytes &= ~1U;
        if (bytes == 0)
        {
            if (!http.connected()) { ok = false; break; }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const size_t got = stream->readBytes(reinterpret_cast<char *>(pcm), bytes);
        if (got == 0 || (got & 1U) != 0) { ok = false; break; }
        received += got;
        last_data_ms = millis();
        if (!audio_write_pcm16_mono(pcm, got / sizeof(int16_t), 250)) { ok = false; break; }
    }
    audio_release_ownership(AUDIO_OWNER_AI_VOICE);
    http.end();
    return ok;
}

void ai_task(void *)
{
    for (;;)
    {
        if (s_process_requested)
        {
            s_process_requested = false;
            s_state = AI_STATE_PROCESSING;
            char transcript[AI_MAX_TEXT_LEN] = {};
            char reply[AI_MAX_TEXT_LEN] = {};
            if (post_recording(transcript, sizeof(transcript), reply, sizeof(reply)))
            {
                ai_voice_add_message(true, transcript);
                ai_voice_add_message(false, reply);
                s_state = AI_STATE_SPEAKING;
                if (!stream_tts_wav(reply)) set_error("TTS HTTPS/WAV playback failed");
                else s_state = AI_STATE_IDLE;
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
    if (s_state == AI_STATE_PROCESSING || s_state == AI_STATE_SPEAKING) return false;
    if (!wifi_manager_is_connected()) { set_error("WiFi is not connected"); return false; }
    if (!audio_start_recording(AUDIO_RECORD_MAX_SEC)) { set_error("Microphone/I2S is busy"); return false; }
    s_recording_started = true;
    s_state = AI_STATE_LISTENING;
    return true;
}

bool ai_voice_stop_and_process(void)
{
    if (s_state != AI_STATE_LISTENING || !s_recording_started) return false;
    audio_stop_recording();
    s_recording_started = false;
    if (audio_get_recorded_sample_count() == 0) { set_error("No audio was recorded"); return false; }
    s_state = AI_STATE_PROCESSING;
    s_process_requested = true;
    return true;
}

void ai_voice_cancel(void)
{
    s_process_requested = false;
    if (s_recording_started)
    {
        audio_stop_recording();
        s_recording_started = false;
    }
    if (s_state == AI_STATE_LISTENING)
        s_state = ai_voice_is_available() ? AI_STATE_IDLE : AI_STATE_ERROR;
}

AIVoiceState ai_voice_get_state(void) { return s_state; }

const char *ai_voice_get_state_text(void)
{
    switch (s_state)
    {
        case AI_STATE_LISTENING: return "Đang thu âm từ microphone...";
        case AI_STATE_PROCESSING: return "Đang gửi HTTPS và xử lý...";
        case AI_STATE_SPEAKING: return "Đang phát TTS WAV...";
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
    if (!configuration_ready()) return false;
    s_state = AI_STATE_SPEAKING;
    const bool ok = stream_tts_wav(text);
    if (ok) s_state = AI_STATE_IDLE;
    else set_error("TTS HTTPS/WAV playback failed");
    return ok;
}
