#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef AI_VOICE_PROVIDER_XIAOZHI
#define AI_VOICE_PROVIDER_XIAOZHI 1
#endif

#if AI_VOICE_PROVIDER_XIAOZHI

#include "ai_voice_service.h"
#include "xiaozhi_activation.h"
#include "xiaozhi_audio_codec.h"
#include "xiaozhi_mcp.h"
#include "xiaozhi_transport.h"
#include "../audio/audio_manager.h"
#include "../audio/music_player.h"
#include "../os/wifi_manager.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifndef XIAOZHI_WSS_CA_CERT
#define XIAOZHI_WSS_CA_CERT ""
#endif

namespace
{
enum class CommandType : uint8_t { START, STOP, CANCEL, RETRY_ACTIVATION, CANCEL_ACTIVATION };
struct Command { CommandType type; uint32_t generation; };

static SemaphoreHandle_t s_mutex = nullptr;
static QueueHandle_t s_commands = nullptr;
static TaskHandle_t s_task = nullptr;
static AIVoiceState s_state = AI_STATE_ERROR;
static ChatMessage s_history[AI_MAX_CHAT_MESSAGES] = {};
static int s_message_count = 0;
static char s_last_error[160] = "Xiaozhi chưa khởi tạo";
static char s_activation_code[32] = {};
static char s_activation_message[160] = {};
static bool s_activation_cancelled = false;
static bool s_configured = false;
static xiaozhi::ProvisionedWebsocket s_ws_config = {};
static uint32_t s_next_generation = 0;
static uint32_t s_active_generation = 0;
static uint32_t s_cancelled_through = 0;

static XiaozhiTransport s_transport;
static XiaozhiAudioCodec s_codec;
static XiaozhiMcpServer s_mcp;
static char s_device_id[18] = {};
static char s_client_id[37] = {};
static bool s_server_hello = false;
static char s_session_id[96] = {};
static uint32_t s_downlink_rate = 16000;
static uint32_t s_session_deadline_ms = 0;
static uint32_t s_seen_uplink_drops = 0;
static uint32_t s_seen_downlink_drops = 0;
static size_t s_capture_offset = 0;
static size_t s_capture_frame_fill = 0;
static MusicVoiceHandoff s_music_handoff = {};
static bool s_music_resume_suppressed = false;
static uint32_t s_audio_output_session = 0;
static uint8_t *s_inbound = nullptr;
static int16_t *s_capture_frame = nullptr;
static int16_t *s_decoded = nullptr;
static int16_t *s_resampled = nullptr;

static const size_t kDecodedCapacity = 5760;
static const size_t kResampledCapacity = 1920;
static const uint32_t kSessionTimeoutMs = 60000;

bool elapsed(uint32_t deadline) { return xiaozhi::deadline_reached(millis(), deadline); }

void set_state(AIVoiceState state)
{
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_state = state;
        xSemaphoreGive(s_mutex);
    }
}

void set_error(const char *message)
{
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        strlcpy(s_last_error, message ? message : "Lỗi Xiaozhi", sizeof(s_last_error));
        s_state = AI_STATE_ERROR;
        xSemaphoreGive(s_mutex);
    }
}

bool current_generation(uint32_t generation)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const bool current = xiaozhi::session_event_is_current(
        generation, s_active_generation, s_cancelled_through);
    xSemaphoreGive(s_mutex);
    return current;
}

void add_message(bool user, const char *text)
{
    if (!text || !*text || !s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;
    if (s_message_count == AI_MAX_CHAT_MESSAGES)
    {
        memmove(s_history, s_history + 1, sizeof(ChatMessage) * (AI_MAX_CHAT_MESSAGES - 1));
        --s_message_count;
    }
    ChatMessage &message = s_history[s_message_count++];
    message.is_user = user;
    strlcpy(message.text, text, sizeof(message.text));
    message.timestamp_sec = millis() / 1000U;
    xSemaphoreGive(s_mutex);
}

bool execute_music(AiMusicActionType type, const char *source = nullptr, uint8_t volume = 0)
{
    AiMusicAction action = {};
    action.type = type;
    action.volume = volume;
    if (source) strlcpy(action.source_id, source, sizeof(action.source_id));
    char error[128] = {};
    return music_player_execute_ai_action(&action, 2500, error, sizeof(error));
}

void stop_output(bool drain)
{
    if (!s_audio_output_session) return;
    if (drain) (void)audio_drain_tx(300);
    (void)audio_set_pa_for_session(AUDIO_OWNER_AI_VOICE, s_audio_output_session, false);
    (void)audio_release_ownership_session(AUDIO_OWNER_AI_VOICE, s_audio_output_session);
    s_audio_output_session = 0;
}

void resume_music_if_needed()
{
    if (s_music_handoff.valid && !s_music_resume_suppressed)
    {
        char error[128] = {};
        if (!music_player_restore_after_voice(&s_music_handoff, 4000, error, sizeof(error)))
            add_message(false, error[0] ? error : "Không thể tiếp tục nhạc");
    }
    memset(&s_music_handoff, 0, sizeof(s_music_handoff));
    s_music_resume_suppressed = false;
}

void finish_session(bool resume_music, bool drain_output = true)
{
    const bool preserve_error = ai_voice_get_state() == AI_STATE_ERROR;
    stop_output(drain_output);
    if (audio_is_recording())
    {
        uint32_t request = 0;
        if (audio_cancel_recording_async(&request))
        {
            bool applied = false;
            (void)audio_wait_recording_command_ack(request, 1200, &applied);
        }
    }
    s_transport.close();
    s_codec.end();
    s_mcp.resetSession();
    s_server_hello = false;
    s_session_id[0] = '\0';
    s_capture_offset = 0;
    s_capture_frame_fill = 0;
    if (resume_music) resume_music_if_needed();
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_active_generation = 0;
        s_state = preserve_error ? AI_STATE_ERROR
                                 : (s_configured ? AI_STATE_IDLE : AI_STATE_NEEDS_USER_INPUT);
        xSemaphoreGive(s_mutex);
    }
}

void cancel_session(uint32_t generation)
{
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (generation > s_cancelled_through) s_cancelled_through = generation;
        s_state = AI_STATE_CANCELING;
        xSemaphoreGive(s_mutex);
    }
    if (s_transport.connected() && s_session_id[0])
    {
        StaticJsonDocument<256> doc;
        doc["session_id"] = s_session_id;
        doc["type"] = "abort";
        doc["reason"] = "user_cancelled";
        String body;
        serializeJson(doc, body);
        (void)s_transport.sendText(body.c_str());
        for (int i = 0; i < 3; ++i) s_transport.loop();
    }
    finish_session(true, false);
}

bool queue_command(CommandType type, uint32_t generation)
{
    if (!s_commands) return false;
    const Command command = {type, generation};
    return xQueueSend(s_commands, &command, 0) == pdTRUE;
}

bool begin_output()
{
    if (s_audio_output_session) return true;
    if (!audio_request_ownership(AUDIO_OWNER_AI_VOICE)) return false;
    s_audio_output_session = audio_get_owner_session(AUDIO_OWNER_AI_VOICE);
    if (!s_audio_output_session || !audio_codec_configure_for_stream(16000, 256) ||
        !audio_set_pa_for_session(AUDIO_OWNER_AI_VOICE, s_audio_output_session, true))
    {
        if (s_audio_output_session)
            (void)audio_release_ownership_session(AUDIO_OWNER_AI_VOICE, s_audio_output_session);
        else audio_release_ownership(AUDIO_OWNER_AI_VOICE);
        s_audio_output_session = 0;
        return false;
    }
    return true;
}

void handle_music_handoff(const char *tool_name)
{
    if (!tool_name) return;
    if (strcmp(tool_name, "self.music.stop") == 0 ||
        strcmp(tool_name, "self.music.pause") == 0)
    {
        s_music_resume_suppressed = true;
        memset(&s_music_handoff, 0, sizeof(s_music_handoff));
    }
    else if ((strcmp(tool_name, "self.music.play") == 0 ||
              strcmp(tool_name, "self.music.resume") == 0) &&
             music_player_is_playing() && !music_player_is_paused())
    {
        MusicVoiceHandoff handoff = {};
        char error[128] = {};
        if (music_player_suspend_for_voice(&handoff, 3000, error, sizeof(error)))
        {
            s_music_handoff = handoff;
            s_music_resume_suppressed = false;
        }
    }
}

bool handle_text_message(const uint8_t *data, size_t size, uint32_t generation)
{
    if (!current_generation(generation) || !data || size == 0 ||
        size > xiaozhi::kMaxJsonMessageBytes) return false;
    DynamicJsonDocument document(12288);
    if (deserializeJson(document, data, size, DeserializationOption::NestingLimit(10)) ||
        !document.is<JsonObject>()) return false;
    JsonObjectConst root = document.as<JsonObjectConst>();
    const char *type = root["type"] | "";
    if (strcmp(type, "hello") == 0)
    {
        if (strcmp(root["transport"] | "", "websocket") != 0) return false;
        JsonObjectConst params = root["audio_params"].as<JsonObjectConst>();
        const char *format = params["format"] | "opus";
        const int channels = params["channels"] | 1;
        const uint32_t rate = params["sample_rate"] | 16000U;
        if (strcmp(format, "opus") != 0 || channels != 1 ||
            (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000))
            return false;
        const char *session = root["session_id"] | "";
        if (!*session || strlen(session) >= sizeof(s_session_id)) return false;
        strlcpy(s_session_id, session, sizeof(s_session_id));
        s_downlink_rate = rate;
        char codec_error[96] = {};
        if (!s_codec.setDownlinkSampleRate(rate, codec_error, sizeof(codec_error)))
        {
            set_error(codec_error);
            return false;
        }
        s_server_hello = true;
        return true;
    }
    const char *session = root["session_id"] | "";
    if (!s_server_hello || !*session || strcmp(session, s_session_id) != 0) return false;
    if (strcmp(type, "stt") == 0)
    {
        const char *text = root["text"] | "";
        if (*text && strlen(text) < AI_MAX_TEXT_LEN) add_message(true, text);
    }
    else if (strcmp(type, "tts") == 0)
    {
        const char *state = root["state"] | "";
        if (strcmp(state, "start") == 0)
        {
            if (!begin_output()) { set_error("Không lấy được I2S để phát Xiaozhi"); return false; }
            set_state(AI_STATE_SPEAKING);
        }
        else if (strcmp(state, "sentence_start") == 0)
        {
            const char *text = root["text"] | "";
            if (*text && strlen(text) < AI_MAX_TEXT_LEN) add_message(false, text);
        }
        else if (strcmp(state, "stop") == 0)
        {
            finish_session(true);
        }
    }
    else if (strcmp(type, "mcp") == 0)
    {
        JsonObjectConst payload = root["payload"].as<JsonObjectConst>();
        const char *tool_name = payload["params"]["name"] | "";
        String response;
        if (s_mcp.handle(payload, s_session_id, response))
        {
            if (strstr(tool_name, "self.music.") == tool_name) handle_music_handoff(tool_name);
            if (!s_transport.sendText(response.c_str())) set_error("Không gửi được MCP ACK");
        }
    }
    else if (strcmp(type, "alert") == 0)
    {
        const char *message = root["message"] | "";
        if (*message && strlen(message) < AI_MAX_TEXT_LEN) add_message(false, message);
    }
    // Remote system/custom commands are intentionally not implemented.
    return true;
}

bool handle_audio_message(const uint8_t *data, size_t size, uint32_t generation)
{
    if (!current_generation(generation) || ai_voice_get_state() != AI_STATE_SPEAKING) return false;
    const uint8_t *opus = nullptr;
    size_t opus_size = 0;
    uint32_t timestamp = 0;
    if (!xiaozhi::unwrap_opus_packet(s_ws_config.version, data, size,
                                      &opus, &opus_size, &timestamp)) return false;
    (void)timestamp;
    const int decoded = s_codec.decode(opus, opus_size, s_decoded, kDecodedCapacity);
    if (decoded <= 0) return false;
    const size_t output_count = xiaozhi_resample_to_16k(
        s_decoded, static_cast<size_t>(decoded), s_downlink_rate,
        s_resampled, kResampledCapacity);
    return output_count > 0 && audio_write_pcm16_mono(s_resampled, output_count, 250);
}

void process_inbound()
{
    XiaozhiInboundKind kind = XiaozhiInboundKind::TEXT;
    size_t size = 0;
    uint32_t generation = 0;
    while (s_transport.receive(&kind, s_inbound, xiaozhi::kMaxJsonMessageBytes,
                               &size, &generation))
    {
        if (!current_generation(generation)) continue;
        const bool ok = kind == XiaozhiInboundKind::TEXT
            ? handle_text_message(s_inbound, size, generation)
            : handle_audio_message(s_inbound, size, generation);
        if (!ok && kind == XiaozhiInboundKind::BINARY &&
            ai_voice_get_state() == AI_STATE_SPEAKING)
            set_error("Gói Opus Xiaozhi lỗi hoặc phát I2S thất bại");
    }
}

bool wait_with_transport(uint32_t deadline, bool (*predicate)())
{
    while (!elapsed(deadline))
    {
        s_transport.loop();
        process_inbound();
        if (predicate()) return true;
        if (!current_generation(s_transport.generation())) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

bool transport_connected() { return s_transport.connected(); }
bool hello_received() { return s_server_hello; }

bool connect_session(uint32_t generation)
{
    char codec_error[96] = {};
    if (!s_codec.begin(16000, codec_error, sizeof(codec_error)))
    {
        set_error(codec_error);
        return false;
    }
    for (uint8_t attempt = 0; attempt < 3 && current_generation(generation); ++attempt)
    {
        char error[128] = {};
        if (!s_transport.begin(s_ws_config, XIAOZHI_WSS_CA_CERT,
                               s_device_id, s_client_id, generation,
                               error, sizeof(error)))
        {
            set_error(error);
            return false;
        }
        if (wait_with_transport(millis() + 10000U, transport_connected))
        {
            StaticJsonDocument<512> hello;
            hello["type"] = "hello";
            hello["version"] = s_ws_config.version;
            hello.createNestedObject("features")["mcp"] = true;
            hello["transport"] = "websocket";
            JsonObject audio = hello.createNestedObject("audio_params");
            audio["format"] = "opus";
            audio["sample_rate"] = 16000;
            audio["channels"] = 1;
            audio["frame_duration"] = 60;
            String body;
            serializeJson(hello, body);
            if (s_transport.sendText(body.c_str()) &&
                wait_with_transport(millis() + 10000U, hello_received))
            {
                s_seen_uplink_drops = s_transport.droppedUplink();
                s_seen_downlink_drops = s_transport.droppedDownlink();
                return true;
            }
        }
        s_transport.close();
        const uint32_t backoff = 500U << attempt;
        const uint32_t until = millis() + backoff;
        while (!elapsed(until) && current_generation(generation)) vTaskDelay(pdMS_TO_TICKS(20));
    }
    set_error("Không kết nối/nhận hello WSS Xiaozhi");
    return false;
}

bool send_listen_state(const char *state)
{
    StaticJsonDocument<256> document;
    document["session_id"] = s_session_id;
    document["type"] = "listen";
    document["state"] = state;
    if (strcmp(state, "start") == 0) document["mode"] = "manual";
    String body;
    serializeJson(document, body);
    return s_transport.sendText(body.c_str());
}

bool encode_capture_frame(uint32_t generation)
{
    uint8_t opus[xiaozhi::kMaxOpusPacketBytes] = {};
    const int encoded = s_codec.encode60ms(s_capture_frame, opus, sizeof(opus));
    if (encoded <= 0) return false;
    uint8_t framed[xiaozhi::kMaxOpusPacketBytes + 16] = {};
    const size_t framed_size = xiaozhi::wrap_opus_packet(
        s_ws_config.version, millis(), opus, static_cast<size_t>(encoded),
        framed, sizeof(framed));
    return framed_size > 0 && s_transport.queueAudio(framed, framed_size, generation);
}

bool pump_capture(uint32_t generation)
{
    int16_t chunk[256];
    size_t total = 0;
    do
    {
        const size_t wanted = sizeof(chunk) / sizeof(chunk[0]);
        const size_t copied = audio_copy_live_recording_samples(
            s_capture_offset, chunk, wanted, &total);
        if (copied == 0) break;
        s_capture_offset += copied;
        size_t consumed = 0;
        while (consumed < copied)
        {
            size_t amount = 960 - s_capture_frame_fill;
            if (amount > copied - consumed) amount = copied - consumed;
            memcpy(s_capture_frame + s_capture_frame_fill, chunk + consumed,
                   amount * sizeof(int16_t));
            s_capture_frame_fill += amount;
            consumed += amount;
            if (s_capture_frame_fill == 960)
            {
                if (!encode_capture_frame(generation)) return false;
                s_capture_frame_fill = 0;
            }
        }
    } while (s_capture_offset < total);
    return true;
}

bool stop_capture_and_send(uint32_t generation, bool cancel)
{
    uint32_t request = 0;
    const bool queued = cancel ? audio_cancel_recording_async(&request)
                               : audio_stop_recording_async(&request);
    bool applied = false;
    if (!queued || !audio_wait_recording_command_ack(request, 1500, &applied) || !applied)
        return false;
    if (!cancel)
    {
        AudioRecordingLease lease = {};
        if (audio_acquire_recording_lease(&lease))
        {
            while (s_capture_offset < lease.sample_count)
            {
                size_t amount = lease.sample_count - s_capture_offset;
                if (amount > 960 - s_capture_frame_fill) amount = 960 - s_capture_frame_fill;
                const size_t copied = audio_copy_recording_lease(
                    &lease, s_capture_offset, s_capture_frame + s_capture_frame_fill, amount);
                if (copied == 0) break;
                s_capture_offset += copied;
                s_capture_frame_fill += copied;
                if (s_capture_frame_fill == 960)
                {
                    if (!encode_capture_frame(generation))
                    {
                        audio_release_recording_lease(&lease);
                        return false;
                    }
                    s_capture_frame_fill = 0;
                }
            }
            audio_release_recording_lease(&lease);
        }
        if (s_capture_frame_fill > 0)
        {
            memset(s_capture_frame + s_capture_frame_fill, 0,
                   (960 - s_capture_frame_fill) * sizeof(int16_t));
            if (!encode_capture_frame(generation)) return false;
            s_capture_frame_fill = 0;
        }
        for (int i = 0; i < 12; ++i) { s_transport.loop(); vTaskDelay(pdMS_TO_TICKS(1)); }
        if (!send_listen_state("stop")) return false;
        set_state(AI_STATE_PROCESSING);
    }
    return true;
}

bool start_session(uint32_t generation)
{
    if (!s_configured || !wifi_manager_is_connected())
    {
        set_error(!s_configured ? "Xiaozhi chưa kích hoạt" : "WiFi chưa kết nối");
        return false;
    }
    memset(&s_music_handoff, 0, sizeof(s_music_handoff));
    s_music_resume_suppressed = false;
    if (music_player_is_playing() || music_player_is_paused())
    {
        char error[128] = {};
        if (!music_player_suspend_for_voice(&s_music_handoff, 3000,
                                            error, sizeof(error)))
        {
            set_error(error[0] ? error : "Không giải phóng được nhạc để thu giọng nói");
            return false;
        }
    }
    if (!connect_session(generation)) { finish_session(true); return false; }
    if (!send_listen_state("start"))
    {
        set_error("Không gửi được trạng thái listen/start");
        finish_session(true);
        return false;
    }
    uint32_t request = 0;
    bool applied = false;
    if (!audio_start_recording_async(AUDIO_RECORD_MAX_SEC, &request) ||
        !audio_wait_recording_command_ack(request, 1500, &applied) || !applied)
    {
        set_error("Không khởi động được microphone");
        finish_session(true);
        return false;
    }
    s_capture_offset = 0;
    s_capture_frame_fill = 0;
    s_session_deadline_ms = millis() + kSessionTimeoutMs;
    set_state(AI_STATE_LISTENING);
    return true;
}

void run_provisioning(uint32_t &next_poll_ms, uint32_t &backoff_ms)
{
    static char last_logged_activation_code[24] = {};
    if (s_configured || s_activation_cancelled || !wifi_manager_is_connected() ||
        !elapsed(next_poll_ms) || s_active_generation) return;
    set_state(AI_STATE_NEEDS_USER_INPUT);
    log_i("Xiaozhi provisioning request: HTTPS OTA discovery");
    XiaozhiProvisionResult result = {};
    char error[160] = {};
    if (xiaozhi_provision_once(&result, error, sizeof(error)))
    {
        backoff_ms = 3000;
        if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            strlcpy(s_activation_code, result.activation_code, sizeof(s_activation_code));
            strlcpy(s_activation_message, result.activation_message, sizeof(s_activation_message));
            if (result.configured)
            {
                s_ws_config = result.websocket;
                s_configured = true;
                s_activation_code[0] = '\0';
                s_activation_message[0] = '\0';
                s_state = AI_STATE_IDLE;
                last_logged_activation_code[0] = '\0';
                log_i("Xiaozhi WSS provisioned: protocol=%u MCP=enabled",
                      static_cast<unsigned>(s_ws_config.version));
            }
            else s_state = AI_STATE_NEEDS_USER_INPUT;
            xSemaphoreGive(s_mutex);
        }
        if (result.activation_required && result.activation_code[0] &&
            strcmp(last_logged_activation_code, result.activation_code) != 0)
        {
            strlcpy(last_logged_activation_code, result.activation_code,
                    sizeof(last_logged_activation_code));
            log_i("Xiaozhi activation required: code=%s", result.activation_code);
        }
        next_poll_ms = millis() + result.poll_after_ms;
    }
    else
    {
        if (error[0])
        {
            log_w("Xiaozhi provisioning failed: %s; retry_ms=%u",
                  error, static_cast<unsigned>(backoff_ms));
            if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
            {
                strlcpy(s_last_error, error, sizeof(s_last_error));
                s_state = AI_STATE_NEEDS_USER_INPUT;
                xSemaphoreGive(s_mutex);
            }
        }
        next_poll_ms = millis() + backoff_ms;
        if (backoff_ms < 60000U) backoff_ms *= 2U;
        if (backoff_ms > 60000U) backoff_ms = 60000U;
    }
}

void worker(void *)
{
    uint32_t next_poll_ms = 0;
    uint32_t backoff_ms = 3000;
    while (true)
    {
        run_provisioning(next_poll_ms, backoff_ms);
        Command command = {};
        while (s_commands && xQueueReceive(s_commands, &command, 0) == pdTRUE)
        {
            if (command.type == CommandType::RETRY_ACTIVATION)
            {
                s_activation_cancelled = false;
                next_poll_ms = 0;
                backoff_ms = 3000;
            }
            else if (command.type == CommandType::CANCEL_ACTIVATION)
            {
                s_activation_cancelled = true;
                set_state(AI_STATE_NEEDS_USER_INPUT);
            }
            else if (command.type == CommandType::START)
                (void)start_session(command.generation);
            else if (command.type == CommandType::STOP && current_generation(command.generation))
            {
                if (!stop_capture_and_send(command.generation, false))
                {
                    set_error("Không kết thúc/gửi được âm thanh Xiaozhi");
                    finish_session(true);
                }
            }
            else if (command.type == CommandType::CANCEL)
                cancel_session(command.generation);
        }

        const uint32_t generation = s_active_generation;
        if (generation)
        {
            s_transport.loop();
            process_inbound();
            if (s_transport.droppedUplink() != s_seen_uplink_drops ||
                s_transport.droppedDownlink() != s_seen_downlink_drops)
            {
                set_error("WebSocket Xiaozhi quá tải hoặc mất gói");
                cancel_session(generation);
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            if (!current_generation(generation))
            {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            const AIVoiceState state = ai_voice_get_state();
            if (state == AI_STATE_LISTENING)
            {
                if (!pump_capture(generation))
                {
                    set_error("Uplink Opus quá tải hoặc encode lỗi");
                    cancel_session(generation);
                }
                else if (!audio_is_recording())
                {
                    if (!stop_capture_and_send(generation, false))
                    {
                        set_error("Microphone dừng ngoài dự kiến");
                        cancel_session(generation);
                    }
                }
            }
            if (generation && elapsed(s_session_deadline_ms))
            {
                set_error("Phiên Xiaozhi quá thời gian");
                cancel_session(generation);
            }
            else if (generation && !s_transport.connected() &&
                     state != AI_STATE_STARTING && state != AI_STATE_CANCELING)
            {
                set_error("WebSocket Xiaozhi đã ngắt");
                cancel_session(generation);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
}

bool ai_voice_init(void)
{
    if (s_task) return true;
    s_mutex = xSemaphoreCreateMutex();
    s_commands = xQueueCreate(8, sizeof(Command));
    s_inbound = static_cast<uint8_t *>(heap_caps_malloc(
        xiaozhi::kMaxJsonMessageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_capture_frame = static_cast<int16_t *>(heap_caps_malloc(960 * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_decoded = static_cast<int16_t *>(heap_caps_malloc(kDecodedCapacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_resampled = static_cast<int16_t *>(heap_caps_malloc(kResampledCapacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_mutex || !s_commands || !s_inbound || !s_capture_frame || !s_decoded || !s_resampled)
    {
        strlcpy(s_last_error, "Thiếu RAM/mutex/queue cho Xiaozhi", sizeof(s_last_error));
        s_state = AI_STATE_ERROR;
        return false;
    }
    if (!xiaozhi_identity_init(s_device_id, sizeof(s_device_id),
                               s_client_id, sizeof(s_client_id)))
    {
        strlcpy(s_last_error, "Không tạo được Device-Id/Client-Id Xiaozhi", sizeof(s_last_error));
        s_state = AI_STATE_ERROR;
        return false;
    }
    s_configured = xiaozhi_load_websocket_config(&s_ws_config);
    s_state = s_configured ? AI_STATE_IDLE : AI_STATE_NEEDS_USER_INPUT;
    strlcpy(s_last_error, s_configured ? "" : "Đang chờ kích hoạt Xiaozhi",
            sizeof(s_last_error));
    if (xTaskCreatePinnedToCore(worker, "XiaozhiVoice", 18432, nullptr, 3,
                                &s_task, 0) != pdPASS)
    {
        s_task = nullptr;
        s_state = AI_STATE_ERROR;
        strlcpy(s_last_error, "Không tạo được Xiaozhi worker", sizeof(s_last_error));
        return false;
    }
    add_message(false, s_configured ? "Xiaozhi đã sẵn sàng. Giữ nút để nói."
                                    : "Đang kết nối dịch vụ kích hoạt Xiaozhi...");
    log_i("Xiaozhi service ready: provisioned=%s MCP=enabled",
          s_configured ? "yes" : "no");
    return true;
}

bool ai_voice_is_available(void) { return s_task != nullptr; }
const char *ai_voice_get_last_error(void) { return s_last_error; }

bool ai_voice_start_recording(void)
{
    if (!s_task || !s_configured || !wifi_manager_is_connected())
    {
        set_error(!s_configured ? "Xiaozhi chưa kích hoạt" : "WiFi chưa kết nối");
        return false;
    }
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    if (s_state != AI_STATE_IDLE || s_active_generation != 0)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    const uint32_t generation = ++s_next_generation;
    s_active_generation = generation;
    s_state = AI_STATE_STARTING;
    xSemaphoreGive(s_mutex);
    if (!queue_command(CommandType::START, generation))
    {
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_active_generation = 0;
            s_state = AI_STATE_ERROR;
            strlcpy(s_last_error, "Queue Xiaozhi đầy", sizeof(s_last_error));
            xSemaphoreGive(s_mutex);
        }
        return false;
    }
    return true;
}

bool ai_voice_stop_and_process(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    const uint32_t generation = s_active_generation;
    const bool allowed = generation && s_state == AI_STATE_LISTENING;
    if (allowed) s_state = AI_STATE_PROCESSING;
    xSemaphoreGive(s_mutex);
    if (!allowed || !queue_command(CommandType::STOP, generation))
    {
        if (allowed) set_error("Queue Xiaozhi đầy khi dừng thu");
        return false;
    }
    return true;
}

void ai_voice_cancel(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    const uint32_t generation = s_active_generation;
    if (generation > s_cancelled_through) s_cancelled_through = generation;
    if (generation) s_state = AI_STATE_CANCELING;
    xSemaphoreGive(s_mutex);
    if (generation && !queue_command(CommandType::CANCEL, generation))
        set_error("Queue Xiaozhi đầy khi hủy");
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
        case AI_STATE_STARTING: return "Đang kết nối Xiaozhi...";
        case AI_STATE_LISTENING: return "Đang nghe và gửi Opus...";
        case AI_STATE_PROCESSING: return "Xiaozhi đang xử lý...";
        case AI_STATE_SPEAKING: return "Xiaozhi đang trả lời...";
        case AI_STATE_CANCELING: return "Đang hủy phiên Xiaozhi...";
        case AI_STATE_NEEDS_USER_INPUT: return s_activation_code[0] ? "Nhập mã kích hoạt Xiaozhi" : s_last_error;
        case AI_STATE_ERROR: return s_last_error;
        default: return "Giữ nút để nói với Xiaozhi";
    }
}

int ai_voice_get_message_count(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    const int count = s_message_count;
    xSemaphoreGive(s_mutex);
    return count;
}

bool ai_voice_get_message_copy(int index, ChatMessage *message)
{
    if (!message || index < 0 || !s_mutex ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    const bool ok = index < s_message_count;
    if (ok) *message = s_history[index];
    xSemaphoreGive(s_mutex);
    return ok;
}

void ai_voice_add_message(bool is_user, const char *text) { add_message(is_user, text); }

void ai_voice_clear_history(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    memset(s_history, 0, sizeof(s_history));
    s_message_count = 0;
    xSemaphoreGive(s_mutex);
}

bool ai_voice_play_tts(const char *)
{
    set_error("Xiaozhi không cung cấp API TTS văn bản riêng trên thiết bị");
    return false;
}

bool ai_voice_get_activation(char *code, size_t code_size,
                             char *message, size_t message_size)
{
    if (!code || !code_size || !message || !message_size || !s_mutex ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    strlcpy(code, s_activation_code, code_size);
    strlcpy(message, s_activation_message, message_size);
    const bool present = s_activation_code[0] != '\0';
    xSemaphoreGive(s_mutex);
    return present;
}

bool ai_voice_retry_activation(void)
{
    return queue_command(CommandType::RETRY_ACTIVATION, 0);
}

bool ai_voice_cancel_activation(void)
{
    return queue_command(CommandType::CANCEL_ACTIVATION, 0);
}

bool ai_voice_json_regression_test(void)
{
    uint8_t opus[] = {1, 2, 3, 4};
    uint8_t framed[32] = {};
    const uint8_t *decoded = nullptr;
    size_t decoded_size = 0;
    uint32_t timestamp = 0;
    const size_t framed_size = xiaozhi::wrap_opus_packet(2, 1234, opus, sizeof(opus),
                                                         framed, sizeof(framed));
    return framed_size == 20 && xiaozhi::unwrap_opus_packet(
        2, framed, framed_size, &decoded, &decoded_size, &timestamp) &&
        decoded_size == sizeof(opus) && timestamp == 1234 &&
        memcmp(decoded, opus, sizeof(opus)) == 0;
}

#endif
