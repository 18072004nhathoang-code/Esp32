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
#include "xiaozhi_session_logic.h"
#include "../audio/audio_manager.h"
#include "../audio/music_player.h"
#include "../camera/camera_service.h"
#include "../os/wifi_manager.h"
#include "../ui/ui_manager.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cmath>

#ifndef XIAOZHI_WSS_CA_CERT
#define XIAOZHI_WSS_CA_CERT ""
#endif

#ifndef FW_GIT_SHA
#define FW_GIT_SHA "unknown"
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
static uint32_t s_history_revision = 0;
static uint32_t s_clear_count = 0;
static uint32_t s_message_seq_id = 0;
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
static xiaozhi::SessionPhaseTracker s_phase_tracker;
static xiaozhi::PhaseDeadlines s_deadlines;
static xiaozhi::SessionTiming s_timing;
static uint32_t s_total_frames_sent = 0;
static uint32_t s_mic_total_samples = 0;
static int16_t s_mic_peak = 0;
static uint64_t s_mic_sum_sq = 0;
static uint32_t s_frames_encoded = 0;
static uint32_t s_frames_enqueued = 0;
#ifndef XIAOZHI_ENABLE_WARM_REUSE
#define XIAOZHI_ENABLE_WARM_REUSE 1
#endif
static bool s_warm_connected = false;
static uint32_t s_idle_since_ms = 0;
static bool s_tts_stopping = false;
static uint32_t s_tts_stop_ms = 0;
static TaskHandle_t s_mcp_task = nullptr;
static QueueHandle_t s_mcp_jobs = nullptr;
static QueueHandle_t s_mcp_results = nullptr;
static uint32_t s_seen_uplink_drops = 0;
static uint32_t s_seen_downlink_drops = 0;
static size_t s_capture_offset = 0;
static size_t s_capture_frame_fill = 0;
static MusicVoiceHandoff s_music_handoff = {};
static bool s_music_resume_suppressed = false;
static uint32_t s_audio_output_session = 0;
static uint32_t s_record_control_request = 0;
static uint8_t *s_inbound = nullptr;
static int16_t *s_capture_frame = nullptr;
static int16_t *s_decoded = nullptr;
static int16_t *s_resampled = nullptr;
static int16_t *s_capture_chunk = nullptr;
static uint8_t *s_opus_packet = nullptr;
static uint8_t *s_framed_packet = nullptr;
static AudioRecordingLease s_flush_lease = {};
static bool s_flush_active = false;
static bool s_flush_source_done = false;
static bool s_cleanup_pending = false;
static bool s_cleanup_resume_music = false;
static bool s_cleanup_preserve_error = false;
static bool s_cleanup_timeout_reported = false;
static uint32_t s_cleanup_generation = 0;
static uint32_t s_cleanup_deadline_ms = 0;
static uint32_t s_cleanup_recorder_request = 0;
static uint8_t s_cleanup_retries = 0;
static uint32_t s_last_diagnostic_ms = 0;
static xiaozhi::BackpressureWindow s_backpressure;

static const size_t kDecodedCapacity = 5760;
static const size_t kResampledCapacity = 1920;
static const uint32_t kBackpressureTimeoutMs = 1200;
static const uint32_t kCleanupTimeoutMs = 5000;
static const size_t kCaptureFrameSamples = 960;
static const size_t kCaptureChunkSamples = 256;
static const size_t kFramesPerWorkerPass = 2;

enum class EncodeResult : uint8_t { QUEUED, BACKPRESSURE, TIMEOUT, ERROR, CANCELLED };
enum class PumpResult : uint8_t { PROGRESS, IDLE, COMPLETE, BACKPRESSURE, TIMEOUT, ERROR, CANCELLED };

bool elapsed(uint32_t deadline) { return xiaozhi::deadline_reached(millis(), deadline); }

void log_session_fault(const char *reason, uint32_t generation)
{
    const uint32_t now = millis();
    const uint32_t elapsed_ms = s_phase_tracker.session_started_ms() ? (now - s_phase_tracker.session_started_ms()) : 0;
    const uint32_t last_prog_ms = s_phase_tracker.last_progress_ms() ? (now - s_phase_tracker.last_progress_ms()) : 0;
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t stack_free = static_cast<size_t>(uxTaskGetStackHighWaterMark(nullptr)) * sizeof(StackType_t);
    log_e("Xiaozhi FAULT [%s] FW=%s gen=%u session=%s phase=%u elapsed=%ums last_prog=%ums up_q=%u down_q=%u drops=%u/%u stack=%uB int=%uB psram=%uB",
          reason ? reason : "Unknown", FW_GIT_SHA,
          static_cast<unsigned>(generation),
          s_session_id[0] ? s_session_id : "none",
          static_cast<unsigned>(s_phase_tracker.phase()),
          static_cast<unsigned>(elapsed_ms), static_cast<unsigned>(last_prog_ms),
          static_cast<unsigned>(s_transport.uplinkPending()),
          static_cast<unsigned>(s_transport.inboundPending()),
          static_cast<unsigned>(s_transport.droppedUplink()),
          static_cast<unsigned>(s_transport.droppedDownlink()),
          static_cast<unsigned>(stack_free),
          static_cast<unsigned>(internal_free),
          static_cast<unsigned>(psram_free));
}

void log_latency_metrics(uint32_t generation)
{
    log_i("Xiaozhi Latency [%s gen=%u]: PTT->WSS=%ums WSS->HELLO=%ums HELLO->REC=%ums REC->REL=%ums REL->LAST_AUDIO=%ums AUDIO->STOP=%ums STOP->STT=%ums STOP->TTS=%ums TTS->PCM=%ums TOTAL=%ums",
          s_session_id[0] ? s_session_id : "none",
          static_cast<unsigned>(generation),
          s_timing.t_wss_connected_ms >= s_timing.t_ptt_ms ? static_cast<unsigned>(s_timing.t_wss_connected_ms - s_timing.t_ptt_ms) : 0,
          s_timing.t_hello_ms >= s_timing.t_wss_connected_ms ? static_cast<unsigned>(s_timing.t_hello_ms - s_timing.t_wss_connected_ms) : 0,
          s_timing.t_recorder_active_ms >= s_timing.t_hello_ms ? static_cast<unsigned>(s_timing.t_recorder_active_ms - s_timing.t_hello_ms) : 0,
          s_timing.t_release_ms >= s_timing.t_recorder_active_ms ? static_cast<unsigned>(s_timing.t_release_ms - s_timing.t_recorder_active_ms) : 0,
          s_timing.t_last_audio_sent_ms >= s_timing.t_release_ms ? static_cast<unsigned>(s_timing.t_last_audio_sent_ms - s_timing.t_release_ms) : 0,
          s_timing.t_listen_stop_ms >= s_timing.t_last_audio_sent_ms ? static_cast<unsigned>(s_timing.t_listen_stop_ms - s_timing.t_last_audio_sent_ms) : 0,
          s_timing.t_stt_ms >= s_timing.t_listen_stop_ms ? static_cast<unsigned>(s_timing.t_stt_ms - s_timing.t_listen_stop_ms) : 0,
          s_timing.t_first_tts_ms >= s_timing.t_listen_stop_ms ? static_cast<unsigned>(s_timing.t_first_tts_ms - s_timing.t_listen_stop_ms) : 0,
          s_timing.t_first_pcm_ms >= s_timing.t_first_tts_ms ? static_cast<unsigned>(s_timing.t_first_pcm_ms - s_timing.t_first_tts_ms) : 0,
          s_timing.t_done_ms >= s_timing.t_ptt_ms ? static_cast<unsigned>(s_timing.t_done_ms - s_timing.t_ptt_ms) : 0);
}

bool current_generation(uint32_t generation);
bool cancellation_requested(uint32_t generation);
bool active_generation_matches(uint32_t generation);
uint32_t active_generation_snapshot();

void mcp_worker(void *)
{
    McpAsyncJob job = {};
    while (true)
    {
        if (s_mcp_jobs && xQueueReceive(s_mcp_jobs, &job, portMAX_DELAY) == pdTRUE)
        {
            if (!current_generation(job.generation) || cancellation_requested(job.generation))
            {
                log_w("Xiaozhi MCP worker: Skipping stale/cancelled job id=%u gen=%u",
                      static_cast<unsigned>(job.id), static_cast<unsigned>(job.generation));
                continue;
            }
            McpAsyncResult res = {};
            res.id = job.id;
            res.generation = job.generation;
            strlcpy(res.session_id, job.session_id, sizeof(res.session_id));
            if (job.tool == xiaozhi::McpTool::MUSIC_PLAY || job.tool == xiaozhi::McpTool::MUSIC_PAUSE ||
                job.tool == xiaozhi::McpTool::MUSIC_RESUME || job.tool == xiaozhi::McpTool::MUSIC_STOP ||
                job.tool == xiaozhi::McpTool::MUSIC_VOLUME)
            {
                char action_error[128] = {};
                const bool device_ack = music_player_execute_ai_action(&job.music_action, 2500,
                                                                       action_error, sizeof(action_error));
                const bool executed = xiaozhi::mcp_result_success(true, device_ack);
                res.is_error = !executed;
                strlcpy(res.text, executed ? "Lệnh nhạc đã được thiết bị ACK" : action_error, sizeof(res.text));
            }
            else if (job.tool == xiaozhi::McpTool::CAMERA_OPEN)
            {
                const bool ok = ui_open_camera_app();
                res.is_error = !ok;
                strlcpy(res.text, ok ? "Camera đã ACK thao tác" : "Camera không thực hiện được thao tác", sizeof(res.text));
            }
            else if (job.tool == xiaozhi::McpTool::CAMERA_START)
            {
                const bool ok = camera_service_start();
                res.is_error = !ok;
                strlcpy(res.text, ok ? "Camera đã ACK thao tác" : "Camera không thực hiện được thao tác", sizeof(res.text));
            }
            else if (job.tool == xiaozhi::McpTool::CAMERA_STOP)
            {
                const bool ok = camera_service_stop(2000);
                res.is_error = !ok;
                strlcpy(res.text, ok ? "Camera đã ACK thao tác" : "Camera không thực hiện được thao tác", sizeof(res.text));
            }
            else if (job.tool == xiaozhi::McpTool::CAMERA_REFRESH)
            {
                const bool stopped = camera_service_stop(2000);
                const bool ok = stopped && camera_service_start();
                res.is_error = !ok;
                strlcpy(res.text, ok ? "Camera đã ACK thao tác" : "Camera không thực hiện được thao tác", sizeof(res.text));
            }
            if (s_mcp_results)
            {
                if (xQueueSend(s_mcp_results, &res, pdMS_TO_TICKS(1000)) != pdTRUE)
                {
                    log_e("Xiaozhi MCP worker: Failed to send MCP result id=%u gen=%u (queue full)",
                          static_cast<unsigned>(res.id), static_cast<unsigned>(res.generation));
                }
            }
        }
    }
}

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

bool cancellation_requested(uint32_t generation)
{
    if (!generation || !s_mutex ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const bool cancelled = xiaozhi::cancellation_applies(generation, s_cancelled_through);
    xSemaphoreGive(s_mutex);
    return cancelled;
}

bool active_generation_matches(uint32_t generation)
{
    if (!generation || !s_mutex ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const bool matches = xiaozhi::cleanup_may_mutate(generation, s_active_generation);
    xSemaphoreGive(s_mutex);
    return matches;
}

uint32_t active_generation_snapshot()
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint32_t generation = s_active_generation;
    xSemaphoreGive(s_mutex);
    return generation;
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
    message.id = ++s_message_seq_id;
    ++s_history_revision;
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

void release_flush_lease()
{
    if (s_flush_lease.token) audio_release_recording_lease(&s_flush_lease);
    s_flush_lease = {};
    s_flush_active = false;
    s_flush_source_done = false;
}

void finalize_cleanup(uint32_t generation)
{
    if (!active_generation_matches(generation)) return;
    release_flush_lease();
    stop_output(false);
    if (s_cleanup_resume_music && !s_music_resume_suppressed && s_state != AI_STATE_CANCELING)
    {
        resume_music_if_needed();
    }
    else
    {
        memset(&s_music_handoff, 0, sizeof(s_music_handoff));
        s_music_resume_suppressed = false;
    }
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (xiaozhi::cleanup_may_mutate(generation, s_active_generation))
        {
            s_active_generation = 0;
            s_state = s_cleanup_preserve_error ? AI_STATE_ERROR
                                               : (s_configured ? AI_STATE_IDLE
                                                               : AI_STATE_NEEDS_USER_INPUT);
        }
        xSemaphoreGive(s_mutex);
    }
    s_cleanup_pending = false;
    s_cleanup_generation = 0;
    s_cleanup_recorder_request = 0;
    s_record_control_request = 0;
    s_cleanup_timeout_reported = false;
    s_cleanup_retries = 0;
}

void service_cleanup()
{
    if (!s_cleanup_pending || !active_generation_matches(s_cleanup_generation)) return;
    bool recorder_stopped = (s_record_control_request == 0 && s_cleanup_recorder_request == 0);
    if (!recorder_stopped && s_cleanup_recorder_request == 0)
    {
        uint32_t request = 0;
        if (s_record_control_request != 0 &&
            audio_cancel_recording_request_async(s_record_control_request, &request))
        {
            s_cleanup_recorder_request = request;
        }
        else
        {
            // The active recording command in audio_manager does not match our request
            const uint32_t active_cmd = audio_get_active_recording_command();
            if (active_cmd != s_record_control_request)
            {
                s_record_control_request = 0;
                recorder_stopped = true;
            }
        }
    }
    if (s_cleanup_recorder_request)
    {
        bool applied = false;
        if (audio_wait_recording_command_ack(s_cleanup_recorder_request, 0, &applied))
        {
            recorder_stopped = applied;
            if (recorder_stopped)
            {
                s_cleanup_recorder_request = 0;
                s_record_control_request = 0;
            }
        }
    }
    if (recorder_stopped)
    {
        finalize_cleanup(s_cleanup_generation);
        return;
    }
    if (elapsed(s_cleanup_deadline_ms))
    {
        if (s_cleanup_retries < 3)
        {
            ++s_cleanup_retries;
            s_cleanup_deadline_ms = millis() + 1500U;
            log_w("Xiaozhi cleanup retry %u generation=%u", s_cleanup_retries, s_cleanup_generation);
            uint32_t retry_req = 0;
            if (s_record_control_request != 0 &&
                audio_cancel_recording_request_async(s_record_control_request, &retry_req))
            {
                s_cleanup_recorder_request = retry_req;
            }
            else
            {
                s_record_control_request = 0;
                s_cleanup_recorder_request = 0;
                recorder_stopped = true;
                finalize_cleanup(s_cleanup_generation);
                return;
            }
        }
        else
        {
            s_cleanup_retries = 0;
            s_cleanup_preserve_error = true;
            set_error("Recorder timeout sau nhiều lần thử hủy");
            s_cleanup_recorder_request = 0;
            s_record_control_request = 0;
            finalize_cleanup(s_cleanup_generation);
        }
    }
}

void begin_cleanup(uint32_t generation, bool resume_music, bool drain_output = true)
{
    if (!active_generation_matches(generation)) return;
    const bool preserve_error = ai_voice_get_state() == AI_STATE_ERROR;
    if (!s_cleanup_pending)
    {
        s_cleanup_pending = true;
        s_cleanup_generation = generation;
        s_cleanup_resume_music = resume_music;
        s_cleanup_preserve_error = preserve_error;
        s_cleanup_deadline_ms = millis() + kCleanupTimeoutMs;
        s_cleanup_timeout_reported = false;
        s_cleanup_retries = 0;
        s_phase_tracker.start_cleanup(millis());
        s_timing.t_done_ms = millis();
        log_i("Xiaozhi: [DONE] gen=%u", static_cast<unsigned>(generation));
        log_latency_metrics(generation);
        s_tts_stopping = false;
        stop_output(drain_output);
        release_flush_lease();
#if XIAOZHI_ENABLE_WARM_REUSE
        if (!preserve_error && !cancellation_requested(generation) && s_transport.connected())
        {
            s_transport.setGeneration(0);
            s_warm_connected = true;
            s_idle_since_ms = millis();
        }
        else
        {
            s_transport.close();
            s_warm_connected = false;
            s_idle_since_ms = 0;
        }
#else
        s_transport.close();
        s_warm_connected = false;
        s_idle_since_ms = 0;
#endif
        s_codec.end();
        s_mcp.resetSession();
        s_server_hello = false;
        s_session_id[0] = '\0';
        s_capture_offset = 0;
        s_capture_frame_fill = 0;
        s_total_frames_sent = 0;
        s_backpressure.reset();
    }
    else
    {
        s_cleanup_resume_music = s_cleanup_resume_music || resume_music;
        s_cleanup_preserve_error = s_cleanup_preserve_error || preserve_error;
    }
    service_cleanup();
}

void cancel_session(uint32_t generation)
{
    if (!active_generation_matches(generation)) return;
    if (s_mutex && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (generation > s_cancelled_through) s_cancelled_through = generation;
        s_state = AI_STATE_CANCELING;
        xSemaphoreGive(s_mutex);
    }
    s_music_resume_suppressed = true;
    stop_output(false);
    release_flush_lease();
    s_capture_offset = 0;
    s_capture_frame_fill = 0;
    s_transport.purgeUplink();
    if (s_record_control_request != 0 && s_cleanup_recorder_request == 0)
    {
        uint32_t request = 0;
        if (audio_cancel_recording_request_async(s_record_control_request, &request))
        {
            s_cleanup_recorder_request = request;
        }
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
    }
    // Never call transport.loop() after abort: queued audio belongs to the
    // cancelled generation and close() discards it.
    begin_cleanup(generation, true, false);
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
    if (!current_generation(generation) || !data || size == 0) return false;
    if (size > xiaozhi::kMaxJsonMessageBytes)
    {
        log_w("Xiaozhi: JSON text message exceeds capacity (%u > %u)",
              static_cast<unsigned>(size), static_cast<unsigned>(xiaozhi::kMaxJsonMessageBytes));
        return false;
    }
    DynamicJsonDocument document(12288);
    const DeserializationError json_err = deserializeJson(
        document, data, size, DeserializationOption::NestingLimit(10));
    if (json_err)
    {
        log_w("Xiaozhi: JSON deserialize error '%s' size=%u gen=%u",
              json_err.c_str(), static_cast<unsigned>(size), static_cast<unsigned>(generation));
        return false;
    }
    if (!document.is<JsonObject>())
    {
        log_w("Xiaozhi: JSON root is not an object size=%u gen=%u",
              static_cast<unsigned>(size), static_cast<unsigned>(generation));
        return false;
    }
    JsonObjectConst root = document.as<JsonObjectConst>();
    const char *type = root["type"] | "";
    if (strcmp(type, "hello") == 0)
    {
        const char *transport_str = root["transport"] | "";
        JsonObjectConst params = root["audio_params"].as<JsonObjectConst>();
        const char *format = params["format"] | "opus";
        const int channels = params["channels"] | 1;
        const uint32_t rate = params["sample_rate"] | 16000U;
        const char *session = root["session_id"] | "";
        const xiaozhi::HelloValidationResult val = xiaozhi::validate_server_hello(
            transport_str, format, channels, rate, session, sizeof(s_session_id));
        if (val != xiaozhi::HelloValidationResult::OK)
        {
            log_w("Xiaozhi hello rejected: %s", xiaozhi::hello_validation_error_string(val));
            return false;
        }
        strlcpy(s_session_id, session, sizeof(s_session_id));
        s_downlink_rate = rate;
        char codec_error[96] = {};
        if (!s_codec.setDownlinkSampleRate(rate, codec_error, sizeof(codec_error)))
        {
            set_error(codec_error);
            return false;
        }
        s_server_hello = true;
        s_timing.t_hello_ms = millis();
        s_phase_tracker.record_progress(s_timing.t_hello_ms);
        log_i("Xiaozhi: [HELLO_OK] session='%s' gen=%u", s_session_id, static_cast<unsigned>(generation));
        return true;
    }

    const char *session = root["session_id"] | "";
    if (!xiaozhi::session_id_matches_contract(session, s_session_id, s_server_hello))
    {
        log_w("Xiaozhi session_id contract failed: session='%s' expected='%s' authenticated=%d",
              session, s_session_id, s_server_hello ? 1 : 0);
        return false;
    }

    if (strcmp(type, "error") == 0)
    {
        const char *msg = root["message"] | root["error"] | "Máy chủ Xiaozhi báo lỗi";
        log_e("Xiaozhi server error: code=%d message='%s'", root["code"].as<int>(), msg);
        set_error(msg);
        begin_cleanup(generation, true, false);
        return true;
    }
    else if (strcmp(type, "abort") == 0)
    {
        const char *reason = root["reason"] | "Máy chủ Xiaozhi đã hủy phiên";
        log_w("Xiaozhi server abort: reason='%s'", reason);
        set_error(reason);
        begin_cleanup(generation, true, false);
        return true;
    }
    else if (strcmp(type, "stt") == 0)
    {
        s_timing.t_stt_ms = millis();
        s_phase_tracker.on_stt_received(s_timing.t_stt_ms);
        const char *text = root["text"] | "";
        log_i("Xiaozhi: [STT_RX] text_len=%u gen=%u", static_cast<unsigned>(strlen(text)), static_cast<unsigned>(generation));
        if (*text && strlen(text) < AI_MAX_TEXT_LEN) add_message(true, text);
    }
    else if (strcmp(type, "tts") == 0)
    {
        const char *state = root["state"] | "";
        if (strcmp(state, "start") == 0)
        {
            if (s_timing.t_first_tts_ms == 0) s_timing.t_first_tts_ms = millis();
            s_phase_tracker.on_tts_start(s_timing.t_first_tts_ms);
            log_i("Xiaozhi: [TTS_START_RX] gen=%u", static_cast<unsigned>(generation));
            if (!begin_output()) { set_error("Không lấy được I2S để phát Xiaozhi"); return false; }
            set_state(AI_STATE_SPEAKING);
        }
        else if (strcmp(state, "sentence_start") == 0)
        {
            if (s_timing.t_first_tts_ms == 0) s_timing.t_first_tts_ms = millis();
            s_phase_tracker.record_progress(millis());
            const char *text = root["text"] | "";
            if (*text && strlen(text) < AI_MAX_TEXT_LEN) add_message(false, text);
        }
        else if (strcmp(state, "stop") == 0)
        {
            s_tts_stopping = true;
            s_tts_stop_ms = millis();
            log_i("Xiaozhi: [TTS_STOP_RX] waiting for downlink queue and audio drain gen=%u", static_cast<unsigned>(generation));
        }
    }
    else if (strcmp(type, "mcp") == 0)
    {
        JsonObjectConst payload = root["payload"].as<JsonObjectConst>();
        if (payload.isNull())
        {
            log_w("Xiaozhi: MCP payload is null gen=%u", static_cast<unsigned>(generation));
            return false;
        }
        const char *tool_name = payload["params"]["name"] | "";
        log_i("Xiaozhi: [MCP_RX] method='%s' tool='%s' gen=%u",
              payload["method"] | "", tool_name, static_cast<unsigned>(generation));
        String response;
        McpAsyncJob job = {};
        const McpDispatchResult disp = s_mcp.dispatch(payload, s_session_id, generation, response, job);
        if (disp == McpDispatchResult::HANDLED_IMMEDIATE)
        {
            if (response.length() > 0)
            {
                if (!s_transport.sendText(response.c_str()))
                {
                    log_w("Xiaozhi: Failed to send immediate MCP response");
                    set_error("Không gửi được MCP ACK");
                }
                else
                {
                    const uint32_t now = millis();
                    s_timing.t_mcp_ack_ms = now;
                    s_phase_tracker.on_mcp_progress(now);
                    log_i("Xiaozhi: [MCP_ACK] immediate sent OK gen=%u", static_cast<unsigned>(generation));
                }
            }
        }
        else if (disp == McpDispatchResult::DISPATCH_ASYNC)
        {
            if (strstr(tool_name, "self.music.") == tool_name) handle_music_handoff(tool_name);
            job.generation = generation;
            if (!s_mcp_jobs || xQueueSend(s_mcp_jobs, &job, pdMS_TO_TICKS(50)) != pdTRUE)
            {
                log_e("Xiaozhi MCP async job queue full for tool '%s'", tool_name);
                String err_resp;
                XiaozhiMcpServer::make_error(job.id, -32000, "Device busy", s_session_id, err_resp);
                if (s_transport.sendText(err_resp.c_str()))
                {
                    s_mcp.remember(job.id, err_resp);
                }
            }
        }
        else if (disp == McpDispatchResult::ERROR_OR_REJECTED)
        {
            log_w("Xiaozhi: MCP request rejected gen=%u (resp_len=%u)",
                  static_cast<unsigned>(generation), static_cast<unsigned>(response.length()));
            if (response.length() > 0)
            {
                (void)s_transport.sendText(response.c_str());
            }
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
    if (output_count > 0 && audio_write_pcm16_mono(s_resampled, output_count, 250))
    {
        const uint32_t now = millis();
        if (s_timing.t_first_pcm_ms == 0)
        {
            s_timing.t_first_pcm_ms = now;
            s_phase_tracker.on_first_pcm(now);
            log_i("Xiaozhi: [FIRST_PCM] gen=%u latency=%ums",
                  static_cast<unsigned>(generation),
                  s_timing.t_first_tts_ms ? static_cast<unsigned>(now - s_timing.t_first_tts_ms) : 0);
        }
        s_phase_tracker.record_progress(now);
        return true;
    }
    return false;
}

void process_inbound()
{
    static const size_t kMaxMessagesPerPass = 4;
    size_t processed = 0;
    XiaozhiInboundKind kind = XiaozhiInboundKind::TEXT;
    size_t size = 0;
    uint32_t generation = 0;
    while (processed < kMaxMessagesPerPass &&
           s_transport.receive(&kind, s_inbound, xiaozhi::kMaxJsonMessageBytes,
                               &size, &generation))
    {
        ++processed;
        if (!current_generation(generation)) continue;
        const bool ok = kind == XiaozhiInboundKind::TEXT
            ? handle_text_message(s_inbound, size, generation)
            : handle_audio_message(s_inbound, size, generation);
        if (!ok && kind == XiaozhiInboundKind::BINARY &&
            ai_voice_get_state() == AI_STATE_SPEAKING)
            set_error("Gói Opus Xiaozhi lỗi hoặc phát I2S thất bại");
        if (current_generation(generation) && ai_voice_get_state() == AI_STATE_ERROR)
            begin_cleanup(generation, true, false);
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
    const uint32_t connect_start_ms = millis();
    const uint32_t connect_budget_deadline = connect_start_ms + s_deadlines.connect_total_budget_ms;
    s_phase_tracker.start_connecting(connect_start_ms);

#if XIAOZHI_ENABLE_WARM_REUSE
    if (s_warm_connected && s_transport.connected() &&
        xiaozhi::can_reuse_connection(s_transport.connected(), s_warm_connected,
                                       s_transport.uplinkPending(), s_transport.inboundPending(),
                                       s_idle_since_ms, connect_start_ms, 30000))
    {
        if (s_transport.setGeneration(generation))
        {
            s_timing.t_wss_connected_ms = millis();
            log_i("Xiaozhi: Reusing warm WSS connection gen=%u epoch=%u",
                  static_cast<unsigned>(generation), static_cast<unsigned>(s_transport.connectionEpoch()));
            const uint32_t warm_deadline = millis() + s_deadlines.warm_connect_timeout_ms;
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
                wait_with_transport(warm_deadline, hello_received))
            {
                s_seen_uplink_drops = s_transport.droppedUplink();
                s_seen_downlink_drops = s_transport.droppedDownlink();
                log_i("Xiaozhi: [HELLO_OK] warm connection established gen=%u", static_cast<unsigned>(generation));
                return true;
            }
            log_w("Xiaozhi: Warm connection hello failed or timed out, falling back to cold connection");
        }
        s_transport.close();
        s_warm_connected = false;
    }
#else
    if (s_transport.connected())
    {
        s_transport.close();
        s_warm_connected = false;
    }
#endif

    for (uint8_t attempt = 0; attempt < 3 && current_generation(generation) && !xiaozhi::deadline_reached(millis(), connect_budget_deadline); ++attempt)
    {
        char error[128] = {};
        if (!s_transport.begin(s_ws_config, XIAOZHI_WSS_CA_CERT,
                               s_device_id, s_client_id, generation,
                               error, sizeof(error)))
        {
            set_error(error);
            return false;
        }
        uint32_t conn_timeout = millis() + 5000U;
        if (conn_timeout > connect_budget_deadline) conn_timeout = connect_budget_deadline;
        if (wait_with_transport(conn_timeout, transport_connected))
        {
            s_timing.t_wss_connected_ms = millis();
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
                wait_with_transport(connect_budget_deadline, hello_received))
            {
                s_seen_uplink_drops = s_transport.droppedUplink();
                s_seen_downlink_drops = s_transport.droppedDownlink();
                log_i("Xiaozhi: [HELLO_OK] cold connection established gen=%u", static_cast<unsigned>(generation));
                return true;
            }
        }
        s_transport.close();
        s_warm_connected = false;
        const uint32_t backoff = 300U << attempt;
        const uint32_t until = millis() + backoff;
        while (!elapsed(until) && current_generation(generation) && !xiaozhi::deadline_reached(millis(), connect_budget_deadline))
        {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    if (!cancellation_requested(generation))
    {
        log_session_fault("Không kết nối/nhận hello WSS Xiaozhi đúng hạn", generation);
        set_error("Không kết nối/nhận hello WSS Xiaozhi");
    }
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

EncodeResult encode_capture_frame(uint32_t generation)
{
    if (!current_generation(generation) || cancellation_requested(generation))
        return EncodeResult::CANCELLED;
    s_transport.loop();
    const bool capacity = s_transport.audioQueueHasCapacity(generation);
    const xiaozhi::BackpressureDecision pressure = s_backpressure.observe(
        capacity, millis(), kBackpressureTimeoutMs);
    if (pressure == xiaozhi::BackpressureDecision::WAIT)
        return EncodeResult::BACKPRESSURE;
    if (pressure == xiaozhi::BackpressureDecision::TIMED_OUT)
        return EncodeResult::TIMEOUT;
    const int encoded = s_codec.encode60ms(
        s_capture_frame, s_opus_packet, xiaozhi::kMaxOpusPacketBytes);
    if (encoded <= 0) return EncodeResult::ERROR;
    ++s_frames_encoded;
    const size_t framed_size = xiaozhi::wrap_opus_packet(
        s_ws_config.version, millis(), s_opus_packet, static_cast<size_t>(encoded),
        s_framed_packet, xiaozhi::kMaxOpusPacketBytes + 16U);
    if (!framed_size) return EncodeResult::ERROR;
    if (!current_generation(generation) || cancellation_requested(generation))
        return EncodeResult::CANCELLED;
    if (!s_transport.queueAudio(s_framed_packet, framed_size, generation))
        return s_transport.audioQueueHasCapacity(generation)
            ? EncodeResult::ERROR : EncodeResult::BACKPRESSURE;
    ++s_frames_enqueued;
    s_backpressure.reset();
    return EncodeResult::QUEUED;
}

PumpResult pump_capture(uint32_t generation)
{
    size_t encoded_frames = 0;
    bool made_progress = false;
    while (encoded_frames < kFramesPerWorkerPass)
    {
        if (cancellation_requested(generation)) return PumpResult::CANCELLED;
        if (s_capture_frame_fill == kCaptureFrameSamples)
        {
            const EncodeResult result = encode_capture_frame(generation);
            if (result == EncodeResult::BACKPRESSURE) return PumpResult::BACKPRESSURE;
            if (result == EncodeResult::TIMEOUT) return PumpResult::TIMEOUT;
            if (result == EncodeResult::CANCELLED) return PumpResult::CANCELLED;
            if (result != EncodeResult::QUEUED) return PumpResult::ERROR;
            s_capture_frame_fill = 0;
            ++encoded_frames;
            made_progress = true;
            continue;
        }
        size_t total = 0;
        size_t wanted = kCaptureFrameSamples - s_capture_frame_fill;
        if (wanted > kCaptureChunkSamples) wanted = kCaptureChunkSamples;
        const size_t copied = audio_copy_live_recording_samples(
            s_capture_offset, s_capture_chunk, wanted, &total, s_record_control_request);
        if (copied == 0) break;
        s_capture_offset += copied;
        for (size_t i = 0; i < copied; ++i)
        {
            const int16_t sample = s_capture_chunk[i];
            const int16_t abs_s = sample < 0 ? (sample == -32768 ? 32767 : -sample) : sample;
            if (abs_s > s_mic_peak) s_mic_peak = abs_s;
            s_mic_sum_sq += static_cast<uint64_t>(sample) * sample;
        }
        s_mic_total_samples += copied;
        size_t consumed = 0;
        while (consumed < copied)
        {
            size_t amount = kCaptureFrameSamples - s_capture_frame_fill;
            if (amount > copied - consumed) amount = copied - consumed;
            memcpy(s_capture_frame + s_capture_frame_fill, s_capture_chunk + consumed,
                   amount * sizeof(int16_t));
            s_capture_frame_fill += amount;
            consumed += amount;
            made_progress = true;
            if (s_capture_frame_fill == kCaptureFrameSamples) break;
        }
    }
    return made_progress ? PumpResult::PROGRESS : PumpResult::IDLE;
}

PumpResult begin_capture_flush(uint32_t generation)
{
    s_timing.t_release_ms = millis();
    s_phase_tracker.start_flushing(s_timing.t_release_ms);
    uint32_t request = 0;
    if (!audio_stop_recording_async(&request)) return PumpResult::ERROR;
    s_record_control_request = request;
    const uint32_t deadline = millis() + 1500U;
    bool applied = false;
    uint32_t snap_gen = 0;
    size_t snap_samples = 0;
    while (!audio_wait_recording_command_ack(request, 0, &applied, &snap_gen, &snap_samples))
    {
        if (cancellation_requested(generation)) return PumpResult::CANCELLED;
        if (elapsed(deadline)) return PumpResult::ERROR;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!applied) return PumpResult::ERROR;
    s_record_control_request = 0;
    release_flush_lease();

    if (!xiaozhi::has_captured_audio(s_capture_offset, s_capture_frame_fill, s_total_frames_sent + snap_samples))
    {
        log_w("Xiaozhi: EMPTY_AUDIO detected, aborting session without waiting for AI");
        set_error("Chưa thu được âm thanh giọng nói");
        begin_cleanup(generation, true, false);
        return PumpResult::ERROR;
    }

    s_flush_active = true;
    if (snap_gen != 0 && snap_samples > 0)
    {
        if (!audio_acquire_recording_lease(&s_flush_lease, snap_gen))
        {
            log_e("Xiaozhi: Failed to acquire snapshot lease for gen %u (%u samples unsent)",
                  static_cast<unsigned>(snap_gen), static_cast<unsigned>(snap_samples));
            set_error("Không truy cập được dữ liệu ghi âm snapshot");
            begin_cleanup(generation, true, false);
            return PumpResult::ERROR;
        }
        s_flush_source_done = false;
    }
    else
    {
        s_flush_source_done = true;
    }
    set_state(AI_STATE_PROCESSING);
    return PumpResult::PROGRESS;
}

PumpResult pump_capture_flush(uint32_t generation)
{
    if (!s_flush_active) return PumpResult::IDLE;
    size_t encoded_frames = 0;
    while (encoded_frames < kFramesPerWorkerPass)
    {
        if (cancellation_requested(generation)) return PumpResult::CANCELLED;
        if (s_capture_frame_fill == kCaptureFrameSamples)
        {
            const EncodeResult result = encode_capture_frame(generation);
            if (result == EncodeResult::BACKPRESSURE) return PumpResult::BACKPRESSURE;
            if (result == EncodeResult::TIMEOUT) return PumpResult::TIMEOUT;
            if (result == EncodeResult::CANCELLED) return PumpResult::CANCELLED;
            if (result != EncodeResult::QUEUED) return PumpResult::ERROR;
            s_capture_frame_fill = 0;
            ++encoded_frames;
            continue;
        }
        if (!s_flush_source_done && s_capture_offset < s_flush_lease.sample_count)
        {
            size_t amount = s_flush_lease.sample_count - s_capture_offset;
            const size_t room = kCaptureFrameSamples - s_capture_frame_fill;
            if (amount > room) amount = room;
            const size_t copied = audio_copy_recording_lease(
                &s_flush_lease, s_capture_offset,
                s_capture_frame + s_capture_frame_fill, amount);
            if (copied == 0) return PumpResult::ERROR;
            for (size_t i = 0; i < copied; ++i)
            {
                const int16_t sample = (s_capture_frame + s_capture_frame_fill)[i];
                const int16_t abs_s = sample < 0 ? (sample == -32768 ? 32767 : -sample) : sample;
                if (abs_s > s_mic_peak) s_mic_peak = abs_s;
                s_mic_sum_sq += static_cast<uint64_t>(sample) * sample;
            }
            s_mic_total_samples += copied;
            s_capture_offset += copied;
            s_capture_frame_fill += copied;
            continue;
        }
        s_flush_source_done = true;
        if (s_capture_frame_fill > 0)
        {
            memset(s_capture_frame + s_capture_frame_fill, 0,
                   (kCaptureFrameSamples - s_capture_frame_fill) * sizeof(int16_t));
            s_capture_frame_fill = kCaptureFrameSamples;
            continue;
        }
        if (s_flush_lease.token) audio_release_recording_lease(&s_flush_lease);
        s_flush_lease = {};
        break;
    }
    if (xiaozhi::flush_ready_for_listen_stop(
            s_flush_source_done && s_capture_frame_fill == 0,
            s_transport.uplinkPending(),
            s_transport.inFlight(),
            s_transport.droppedUplink() != s_seen_uplink_drops))
    {
        uint32_t rms = 0;
        if (s_mic_total_samples > 0)
        {
            rms = static_cast<uint32_t>(sqrt(static_cast<double>(s_mic_sum_sq) / s_mic_total_samples));
        }
        log_i("Xiaozhi: [AUDIO_SENT] samples=%u peak=%d rms=%u enc=%u enq=%u sent=%u bytes=%u drops=%u",
              static_cast<unsigned>(s_mic_total_samples),
              static_cast<int>(s_mic_peak),
              static_cast<unsigned>(rms),
              static_cast<unsigned>(s_frames_encoded),
              static_cast<unsigned>(s_frames_enqueued),
              static_cast<unsigned>(s_transport.framesSent()),
              static_cast<unsigned>(s_transport.bytesSent()),
              static_cast<unsigned>(s_transport.droppedUplink()));

        if (!send_listen_state("stop")) return PumpResult::ERROR;
        log_i("Xiaozhi: [LISTEN_STOP_SENT] gen=%u", static_cast<unsigned>(generation));
        s_flush_active = false;
        s_timing.t_listen_stop_ms = millis();
        s_phase_tracker.start_waiting_response(s_timing.t_listen_stop_ms);
        return PumpResult::COMPLETE;
    }
    return PumpResult::PROGRESS;
}

bool start_session(uint32_t generation)
{
    if (!current_generation(generation)) return false;
    if (!s_configured || !wifi_manager_is_connected())
    {
        set_error(!s_configured ? "Xiaozhi chưa kích hoạt" : "WiFi chưa kết nối");
        begin_cleanup(generation, true, false);
        return false;
    }
    s_timing.reset();
    s_timing.t_ptt_ms = millis();
    s_total_frames_sent = 0;
    s_mic_total_samples = 0;
    s_mic_peak = 0;
    s_mic_sum_sq = 0;
    s_frames_encoded = 0;
    s_frames_enqueued = 0;
    s_transport.resetAudioCounters();
    memset(&s_music_handoff, 0, sizeof(s_music_handoff));
    s_music_resume_suppressed = false;
    if (music_player_is_playing() || music_player_is_paused())
    {
        char error[128] = {};
        if (!music_player_suspend_for_voice(&s_music_handoff, 3000,
                                            error, sizeof(error)))
        {
            set_error(error[0] ? error : "Không giải phóng được nhạc để thu giọng nói");
            begin_cleanup(generation, true, false);
            return false;
        }
    }
    if (cancellation_requested(generation)) { cancel_session(generation); return false; }
    if (!connect_session(generation)) { begin_cleanup(generation, true); return false; }
    if (cancellation_requested(generation)) { cancel_session(generation); return false; }
    if (!send_listen_state("start"))
    {
        set_error("Không gửi được trạng thái listen/start");
        begin_cleanup(generation, true);
        return false;
    }
    log_i("Xiaozhi: [LISTEN_START_SENT] gen=%u", static_cast<unsigned>(generation));
    if (cancellation_requested(generation)) { cancel_session(generation); return false; }
    uint32_t request = 0;
    bool applied = false;
    if (!audio_start_recording_async(AUDIO_RECORD_MAX_SEC, &request))
    {
        set_error("Không khởi động được microphone");
        begin_cleanup(generation, true);
        return false;
    }
    s_record_control_request = request;
    const uint32_t ack_deadline = millis() + 1500U;
    while (!audio_wait_recording_command_ack(request, 0, &applied))
    {
        if (cancellation_requested(generation))
        {
            uint32_t cancel_req = 0;
            if (audio_cancel_recording_request_async(request, &cancel_req))
            {
                s_cleanup_recorder_request = cancel_req;
            }
            cancel_session(generation);
            return false;
        }
        if (elapsed(ack_deadline))
        {
            set_error("Microphone Start không ACK đúng hạn");
            uint32_t cancel_req = 0;
            if (audio_cancel_recording_request_async(request, &cancel_req))
            {
                s_cleanup_recorder_request = cancel_req;
            }
            begin_cleanup(generation, true);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!applied || !current_generation(generation))
    {
        if (!cancellation_requested(generation)) set_error("Không khởi động được microphone");
        uint32_t cancel_req = 0;
        if (audio_cancel_recording_request_async(request, &cancel_req))
        {
            s_cleanup_recorder_request = cancel_req;
        }
        begin_cleanup(generation, true);
        return false;
    }
    s_capture_offset = 0;
    s_capture_frame_fill = 0;
    s_flush_active = false;
    s_flush_source_done = false;
    s_backpressure.reset();
    s_timing.t_recorder_active_ms = millis();
    log_i("Xiaozhi: [RECORDER_ACTIVE] gen=%u", static_cast<unsigned>(generation));
    s_phase_tracker.start_listening(s_timing.t_recorder_active_ms);
    set_state(AI_STATE_LISTENING);
    return true;
}

void run_provisioning(uint32_t &next_poll_ms, uint32_t &backoff_ms)
{
    static char last_logged_activation_code[24] = {};
    if (s_configured || s_activation_cancelled || !wifi_manager_is_connected() ||
        !elapsed(next_poll_ms) || active_generation_snapshot()) return;
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

void log_runtime_diagnostics(uint32_t generation)
{
    if (!generation || millis() - s_last_diagnostic_ms < 15000U) return;
    s_last_diagnostic_ms = millis();
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t stack_free = static_cast<size_t>(uxTaskGetStackHighWaterMark(nullptr)) *
                              sizeof(StackType_t);
    log_i("Xiaozhi diag state=%u gen=%u recorder=%u owner=%u cmdq=%u upq=%u/8 drops=%u/%u stack_free=%uB int=%u/%u/%u psram=%u/%u/%u cleanup=%u",
          static_cast<unsigned>(ai_voice_get_state()), static_cast<unsigned>(generation),
          static_cast<unsigned>(s_record_control_request),
          static_cast<unsigned>(audio_get_current_owner()),
          static_cast<unsigned>(s_commands ? uxQueueMessagesWaiting(s_commands) : 0),
          static_cast<unsigned>(s_transport.uplinkPending()),
          static_cast<unsigned>(s_transport.droppedUplink()),
          static_cast<unsigned>(s_transport.droppedDownlink()),
          static_cast<unsigned>(stack_free),
          static_cast<unsigned>(internal_free), static_cast<unsigned>(internal_min),
          static_cast<unsigned>(internal_largest), static_cast<unsigned>(psram_free),
          static_cast<unsigned>(psram_min), static_cast<unsigned>(psram_largest),
          s_cleanup_pending ? 1U : 0U);
}

bool handle_pump_fault(PumpResult result, uint32_t generation, const char *context)
{
    if (result == PumpResult::CANCELLED)
    {
        cancel_session(generation);
        return true;
    }
    if (result == PumpResult::TIMEOUT || result == PumpResult::ERROR)
    {
        set_error(result == PumpResult::TIMEOUT
                      ? "Uplink Xiaozhi nghẽn quá thời hạn"
                      : context);
        begin_cleanup(generation, true, false);
        return true;
    }
    return false;
}

bool run_codec_self_test()
{
    memset(s_capture_frame, 0, kCaptureFrameSamples * sizeof(int16_t));
    char error[96] = {};
    if (!s_codec.begin(16000, error, sizeof(error)))
    {
        log_e("Xiaozhi Opus self-test init failed: %s", error);
        return false;
    }
    const int encoded = s_codec.encode60ms(
        s_capture_frame, s_opus_packet, xiaozhi::kMaxOpusPacketBytes);
    const int decoded = encoded > 0
        ? s_codec.decode(s_opus_packet, static_cast<size_t>(encoded),
                         s_decoded, kDecodedCapacity)
        : encoded;
    s_codec.end();
    const bool ok = encoded > 0 && decoded == static_cast<int>(kCaptureFrameSamples);
    log_i("Xiaozhi Opus self-test=%s encoded=%d decoded=%d stack_free=%uB",
          ok ? "PASS" : "FAIL", encoded, decoded,
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    return ok;
}

void worker(void *)
{
    uint32_t next_poll_ms = 0;
    uint32_t backoff_ms = 3000;
    if (!run_codec_self_test()) set_error("Opus encode/decode self-test thất bại");
    while (true)
    {
        run_provisioning(next_poll_ms, backoff_ms);
        service_cleanup();

        McpAsyncResult mcp_res = {};
        while (s_mcp_results && xQueueReceive(s_mcp_results, &mcp_res, 0) == pdTRUE)
        {
            if (mcp_res.generation != active_generation_snapshot() ||
                cancellation_requested(mcp_res.generation))
            {
                log_w("Xiaozhi: Dropping stale MCP async result id=%u gen=%u (active=%u)",
                      static_cast<unsigned>(mcp_res.id),
                      static_cast<unsigned>(mcp_res.generation),
                      static_cast<unsigned>(active_generation_snapshot()));
                continue;
            }
            String resp_str;
            XiaozhiMcpServer::make_text_result(mcp_res.id, mcp_res.text, mcp_res.is_error, mcp_res.session_id, resp_str);
            if (s_transport.connected())
            {
                if (s_transport.sendText(resp_str.c_str()))
                {
                    s_mcp.remember(mcp_res.id, resp_str);
                    const uint32_t now = millis();
                    s_timing.t_mcp_ack_ms = now;
                    s_phase_tracker.on_mcp_progress(now);
                    log_i("Xiaozhi: [MCP_ACK] async sent OK id=%u gen=%u",
                          static_cast<unsigned>(mcp_res.id), static_cast<unsigned>(mcp_res.generation));
                }
                else
                {
                    log_w("Xiaozhi: Không gửi được MCP ACK id=%u gen=%u",
                          static_cast<unsigned>(mcp_res.id), static_cast<unsigned>(mcp_res.generation));
                }
            }
        }

        uint32_t generation = active_generation_snapshot();
        if (generation && cancellation_requested(generation))
        {
            cancel_session(generation);
            service_cleanup();
        }
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
            else if (command.type == CommandType::START && current_generation(command.generation))
                (void)start_session(command.generation);
            else if (command.type == CommandType::STOP && current_generation(command.generation))
            {
                const PumpResult result = begin_capture_flush(command.generation);
                (void)handle_pump_fault(result, command.generation,
                                        "Không dừng được recorder Xiaozhi");
            }
            else if (command.type == CommandType::CANCEL)
                cancel_session(command.generation);
        }

        service_cleanup();
        generation = active_generation_snapshot();

        if (s_warm_connected && generation == 0 &&
            static_cast<int32_t>(millis() - s_idle_since_ms) >= 30000)
        {
            log_i("Xiaozhi warm connection idle timeout (30s): đóng socket WSS");
            s_transport.close();
            s_warm_connected = false;
        }

        if (s_tts_stopping && generation && !s_cleanup_pending)
        {
            if (s_transport.inboundIdle() || static_cast<int32_t>(millis() - s_tts_stop_ms) >= 3000)
            {
                audio_drain_tx(500);
                s_tts_stopping = false;
                begin_cleanup(generation, true, true);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
                continue;
            }
        }

        if (generation && !s_cleanup_pending)
        {
            if (cancellation_requested(generation))
            {
                cancel_session(generation);
                service_cleanup();
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
                continue;
            }
            s_transport.loop();
            if (s_transport.lastAudioSentMs() > s_timing.t_last_audio_sent_ms)
            {
                s_timing.t_last_audio_sent_ms = s_transport.lastAudioSentMs();
                s_total_frames_sent = s_transport.framesSent();
                s_phase_tracker.record_progress(s_timing.t_last_audio_sent_ms);
            }
            process_inbound();
            if (s_transport.droppedUplink() != s_seen_uplink_drops ||
                s_transport.droppedDownlink() != s_seen_downlink_drops)
            {
                log_session_fault("WebSocket Xiaozhi quá tải hoặc mất gói", generation);
                set_error("WebSocket Xiaozhi quá tải hoặc mất gói");
                begin_cleanup(generation, true, false);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
                continue;
            }
            if (!current_generation(generation))
            {
                cancel_session(generation);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
                continue;
            }
            const AIVoiceState state = ai_voice_get_state();
            if (state == AI_STATE_LISTENING)
            {
                const PumpResult result = pump_capture(generation);
                if (handle_pump_fault(result, generation, "Opus encode/uplink lỗi"))
                    continue;
                else if (!audio_is_recording())
                {
                    const PumpResult stop_result = begin_capture_flush(generation);
                    if (handle_pump_fault(stop_result, generation,
                                          "Microphone dừng ngoài dự kiến")) continue;
                }
            }
            else if (state == AI_STATE_PROCESSING && s_flush_active)
            {
                const PumpResult result = pump_capture_flush(generation);
                if (handle_pump_fault(result, generation,
                                      "Không flush được audio cuối Xiaozhi")) continue;
            }
            const xiaozhi::SessionPhaseTracker::TimeoutReason timeout_reason =
                s_phase_tracker.check_timeout(millis(), s_deadlines);
            if (timeout_reason != xiaozhi::SessionPhaseTracker::TimeoutReason::NONE)
            {
                const char *err_text = xiaozhi::SessionPhaseTracker::timeout_reason_string(timeout_reason);
                log_session_fault(err_text, generation);
                set_error(err_text);
                begin_cleanup(generation, true, false);
            }
            else if (!s_transport.connected() &&
                      state != AI_STATE_STARTING && state != AI_STATE_CANCELING)
            {
                log_session_fault("WebSocket Xiaozhi đã ngắt kết nối", generation);
                set_error("WebSocket Xiaozhi đã ngắt");
                begin_cleanup(generation, true, false);
            }
            log_runtime_diagnostics(generation);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
    }
}

void release_service_allocations()
{
    if (s_mcp_task)
    {
        vTaskDelete(s_mcp_task);
        s_mcp_task = nullptr;
    }
    if (s_mcp_jobs) vQueueDelete(s_mcp_jobs);
    if (s_mcp_results) vQueueDelete(s_mcp_results);
    if (s_commands) vQueueDelete(s_commands);
    if (s_mutex) vSemaphoreDelete(s_mutex);
    heap_caps_free(s_inbound);
    heap_caps_free(s_capture_frame);
    heap_caps_free(s_decoded);
    heap_caps_free(s_resampled);
    heap_caps_free(s_capture_chunk);
    heap_caps_free(s_opus_packet);
    heap_caps_free(s_framed_packet);
    s_mcp_jobs = nullptr;
    s_mcp_results = nullptr;
    s_commands = nullptr;
    s_mutex = nullptr;
    s_inbound = nullptr;
    s_capture_frame = nullptr;
    s_decoded = nullptr;
    s_resampled = nullptr;
    s_capture_chunk = nullptr;
    s_opus_packet = nullptr;
    s_framed_packet = nullptr;
}
}

bool ai_voice_init(void)
{
    if (s_task) return true;
    s_mutex = xSemaphoreCreateMutex();
    s_commands = xQueueCreate(8, sizeof(Command));
    s_mcp_jobs = xQueueCreate(4, sizeof(McpAsyncJob));
    s_mcp_results = xQueueCreate(4, sizeof(McpAsyncResult));
    s_inbound = static_cast<uint8_t *>(heap_caps_malloc(
        xiaozhi::kMaxJsonMessageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_capture_frame = static_cast<int16_t *>(heap_caps_malloc(960 * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_decoded = static_cast<int16_t *>(heap_caps_malloc(kDecodedCapacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_resampled = static_cast<int16_t *>(heap_caps_malloc(kResampledCapacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_capture_chunk = static_cast<int16_t *>(heap_caps_malloc(
        kCaptureChunkSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_opus_packet = static_cast<uint8_t *>(heap_caps_malloc(
        xiaozhi::kMaxOpusPacketBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_framed_packet = static_cast<uint8_t *>(heap_caps_malloc(
        xiaozhi::kMaxOpusPacketBytes + 16U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_mutex || !s_commands || !s_mcp_jobs || !s_mcp_results ||
        !s_inbound || !s_capture_frame || !s_decoded ||
        !s_resampled || !s_capture_chunk || !s_opus_packet || !s_framed_packet)
    {
        strlcpy(s_last_error, "Thiếu RAM/mutex/queue cho Xiaozhi", sizeof(s_last_error));
        s_state = AI_STATE_ERROR;
        release_service_allocations();
        return false;
    }
    if (!xiaozhi_identity_init(s_device_id, sizeof(s_device_id),
                               s_client_id, sizeof(s_client_id)))
    {
        strlcpy(s_last_error, "Không tạo được Device-Id/Client-Id Xiaozhi", sizeof(s_last_error));
        s_state = AI_STATE_ERROR;
        release_service_allocations();
        return false;
    }
    s_configured = xiaozhi_load_websocket_config(&s_ws_config);
    s_state = s_configured ? AI_STATE_IDLE : AI_STATE_NEEDS_USER_INPUT;
    strlcpy(s_last_error, s_configured ? "" : "Đang chờ kích hoạt Xiaozhi",
            sizeof(s_last_error));
    if (xTaskCreatePinnedToCore(mcp_worker, "XiaozhiMCP", 4096, nullptr, 3,
                                &s_mcp_task, 1) != pdPASS)
    {
        s_mcp_task = nullptr;
        s_state = AI_STATE_ERROR;
        strlcpy(s_last_error, "Không tạo được Xiaozhi MCP worker", sizeof(s_last_error));
        release_service_allocations();
        return false;
    }
    if (xTaskCreatePinnedToCore(worker, "XiaozhiVoice", 32768, nullptr, 3,
                                &s_task, 0) != pdPASS)
    {
        s_task = nullptr;
        s_state = AI_STATE_ERROR;
        strlcpy(s_last_error, "Không tạo được Xiaozhi worker", sizeof(s_last_error));
        release_service_allocations();
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
    if ((s_state != AI_STATE_IDLE && s_state != AI_STATE_ERROR) ||
        s_active_generation != 0)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    uint32_t generation = ++s_next_generation;
    if (generation == 0) generation = ++s_next_generation;
    s_active_generation = generation;
    s_state = AI_STATE_STARTING;
    s_last_error[0] = '\0';
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
    xTaskNotifyGive(s_task);
    return true;
}

bool ai_voice_stop_and_process(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    const uint32_t generation = s_active_generation;
    const bool starting = generation && s_state == AI_STATE_STARTING;
    const bool allowed = generation && s_state == AI_STATE_LISTENING;
    if (starting)
    {
        if (generation > s_cancelled_through) s_cancelled_through = generation;
        s_state = AI_STATE_CANCELING;
    }
    if (allowed) s_state = AI_STATE_PROCESSING;
    xSemaphoreGive(s_mutex);
    if (starting)
    {
        (void)queue_command(CommandType::CANCEL, generation);
        xTaskNotifyGive(s_task);
        return true;
    }
    if (!allowed || !queue_command(CommandType::STOP, generation))
    {
        if (allowed)
        {
            if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
            {
                if (generation > s_cancelled_through) s_cancelled_through = generation;
                s_state = AI_STATE_ERROR;
                strlcpy(s_last_error, "Queue Xiaozhi đầy; hủy phiên an toàn",
                        sizeof(s_last_error));
                xSemaphoreGive(s_mutex);
            }
            xTaskNotifyGive(s_task);
        }
        return false;
    }
    xTaskNotifyGive(s_task);
    return true;
}

void ai_voice_cancel(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    const uint32_t generation = s_active_generation;
    if (generation > s_cancelled_through) s_cancelled_through = generation;
    if (generation) s_state = AI_STATE_CANCELING;
    xSemaphoreGive(s_mutex);
    if (generation)
    {
        // The mutex-protected cancellation watermark is authoritative. The
        // queue entry only reduces latency and may be dropped safely.
        (void)queue_command(CommandType::CANCEL, generation);
        xTaskNotifyGive(s_task);
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

uint32_t ai_voice_get_history_revision(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    const uint32_t rev = s_history_revision;
    xSemaphoreGive(s_mutex);
    return rev;
}

void ai_voice_clear_history(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    memset(s_history, 0, sizeof(s_history));
    s_message_count = 0;
    ++s_history_revision;
    ++s_clear_count;
    xSemaphoreGive(s_mutex);
}

uint32_t ai_voice_get_clear_count(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    const uint32_t count = s_clear_count;
    xSemaphoreGive(s_mutex);
    return count;
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
