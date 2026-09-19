#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace xiaozhi
{
enum class BackpressureDecision : uint8_t
{
    READY = 0,
    WAIT,
    TIMED_OUT
};

class BackpressureWindow
{
public:
    BackpressureWindow() : since_ms_(0) {}

    BackpressureDecision observe(bool queue_has_capacity, uint32_t now_ms,
                                 uint32_t timeout_ms)
    {
        if (queue_has_capacity)
        {
            since_ms_ = 0;
            return BackpressureDecision::READY;
        }
        if (since_ms_ == 0) since_ms_ = now_ms == 0 ? 1 : now_ms;
        return static_cast<int32_t>(now_ms - since_ms_) >=
                       static_cast<int32_t>(timeout_ms)
                   ? BackpressureDecision::TIMED_OUT
                   : BackpressureDecision::WAIT;
    }

    void reset() { since_ms_ = 0; }
    uint32_t sinceMs() const { return since_ms_; }

private:
    uint32_t since_ms_;
};

inline bool cancellation_applies(uint32_t generation, uint32_t cancelled_through)
{
    return generation != 0 && generation <= cancelled_through;
}

inline bool cleanup_may_mutate(uint32_t cleanup_generation,
                               uint32_t active_generation)
{
    return cleanup_generation != 0 && cleanup_generation == active_generation;
}

inline bool recorder_control_targets(uint32_t expected_request,
                                     uint32_t current_request)
{
    return expected_request != 0 && expected_request == current_request;
}

inline bool flush_ready_for_listen_stop(bool capture_complete,
                                        size_t queued_audio,
                                        bool transport_failed)
{
    return capture_complete && queued_audio == 0 && !transport_failed;
}

inline size_t bounded_frame_budget(size_t available_samples,
                                   size_t frame_samples,
                                   size_t max_frames)
{
    if (frame_samples == 0 || max_frames == 0) return 0;
    const size_t frames = (available_samples + frame_samples - 1U) / frame_samples;
    return frames < max_frames ? frames : max_frames;
}

inline bool snapshot_lease_matches(uint32_t expected_gen, uint32_t snapshot_gen)
{
    return expected_gen == 0 || (snapshot_gen != 0 && snapshot_gen == expected_gen);
}

inline bool live_capture_matches(uint32_t expected_cmd, uint32_t active_cmd)
{
    return expected_cmd == 0 || (active_cmd != 0 && active_cmd == expected_cmd);
}

inline bool snapshot_flush_needed(uint32_t snapshot_gen, size_t sample_count)
{
    return snapshot_gen != 0 && sample_count > 0;
}

enum class SessionPhase : uint8_t
{
    IDLE = 0,
    CONNECTING,
    LISTENING,
    FLUSHING,
    WAITING_RESPONSE,
    SPEAKING,
    CLEANUP
};

struct PhaseDeadlines
{
    uint32_t connect_total_budget_ms = 15000;
    uint32_t listening_max_ms = 30000;
    uint32_t flushing_timeout_ms = 5000;
    uint32_t wait_response_timeout_ms = 20000;
    uint32_t speaking_stall_timeout_ms = 8000;
    uint32_t speaking_max_total_ms = 180000;
    uint32_t cleanup_timeout_ms = 5000;
    uint32_t session_absolute_max_ms = 240000;
};

struct SessionTiming
{
    uint32_t t_ptt_ms = 0;
    uint32_t t_wss_connected_ms = 0;
    uint32_t t_hello_ms = 0;
    uint32_t t_recorder_active_ms = 0;
    uint32_t t_release_ms = 0;
    uint32_t t_last_audio_sent_ms = 0;
    uint32_t t_listen_stop_ms = 0;
    uint32_t t_stt_ms = 0;
    uint32_t t_mcp_ack_ms = 0;
    uint32_t t_first_tts_ms = 0;
    uint32_t t_first_pcm_ms = 0;
    uint32_t t_done_ms = 0;

    void reset()
    {
        memset(this, 0, sizeof(*this));
    }
};

class SessionPhaseTracker
{
public:
    SessionPhaseTracker() = default;

    void reset()
    {
        phase_ = SessionPhase::IDLE;
        session_started_ms_ = 0;
        phase_started_ms_ = 0;
        last_progress_ms_ = 0;
    }

    void start_connecting(uint32_t now_ms)
    {
        phase_ = SessionPhase::CONNECTING;
        session_started_ms_ = now_ms;
        phase_started_ms_ = now_ms;
        last_progress_ms_ = now_ms;
    }

    void start_listening(uint32_t now_ms)
    {
        phase_ = SessionPhase::LISTENING;
        phase_started_ms_ = now_ms;
        last_progress_ms_ = now_ms;
    }

    void start_flushing(uint32_t now_ms)
    {
        phase_ = SessionPhase::FLUSHING;
        phase_started_ms_ = now_ms;
        last_progress_ms_ = now_ms;
    }

    void start_waiting_response(uint32_t now_ms)
    {
        phase_ = SessionPhase::WAITING_RESPONSE;
        phase_started_ms_ = now_ms;
        last_progress_ms_ = now_ms;
    }

    void start_speaking(uint32_t now_ms)
    {
        phase_ = SessionPhase::SPEAKING;
        phase_started_ms_ = now_ms;
        last_progress_ms_ = now_ms;
    }

    void start_cleanup(uint32_t now_ms)
    {
        phase_ = SessionPhase::CLEANUP;
        phase_started_ms_ = now_ms;
        last_progress_ms_ = now_ms;
    }

    void record_progress(uint32_t now_ms)
    {
        last_progress_ms_ = now_ms;
    }

    SessionPhase phase() const { return phase_; }
    uint32_t phase_started_ms() const { return phase_started_ms_; }
    uint32_t last_progress_ms() const { return last_progress_ms_; }
    uint32_t session_started_ms() const { return session_started_ms_; }

    enum class TimeoutReason : uint8_t
    {
        NONE = 0,
        CONNECT_TIMEOUT,
        LISTENING_TIMEOUT,
        FLUSH_TIMEOUT,
        WAIT_RESPONSE_TIMEOUT,
        SPEAKING_STALLED,
        SPEAKING_MAX_EXCEEDED,
        CLEANUP_TIMEOUT,
        ABSOLUTE_SESSION_TIMEOUT
    };

    TimeoutReason check_timeout(uint32_t now_ms, const PhaseDeadlines &deadlines) const
    {
        if (phase_ == SessionPhase::IDLE) return TimeoutReason::NONE;

        if (phase_ != SessionPhase::CLEANUP &&
            static_cast<int32_t>(now_ms - session_started_ms_) >= static_cast<int32_t>(deadlines.session_absolute_max_ms))
        {
            return TimeoutReason::ABSOLUTE_SESSION_TIMEOUT;
        }

        switch (phase_)
        {
            case SessionPhase::CONNECTING:
                if (static_cast<int32_t>(now_ms - phase_started_ms_) >= static_cast<int32_t>(deadlines.connect_total_budget_ms))
                    return TimeoutReason::CONNECT_TIMEOUT;
                break;
            case SessionPhase::LISTENING:
                if (static_cast<int32_t>(now_ms - phase_started_ms_) >= static_cast<int32_t>(deadlines.listening_max_ms))
                    return TimeoutReason::LISTENING_TIMEOUT;
                break;
            case SessionPhase::FLUSHING:
                if (static_cast<int32_t>(now_ms - phase_started_ms_) >= static_cast<int32_t>(deadlines.flushing_timeout_ms))
                    return TimeoutReason::FLUSH_TIMEOUT;
                break;
            case SessionPhase::WAITING_RESPONSE:
                if (static_cast<int32_t>(now_ms - phase_started_ms_) >= static_cast<int32_t>(deadlines.wait_response_timeout_ms))
                    return TimeoutReason::WAIT_RESPONSE_TIMEOUT;
                break;
            case SessionPhase::SPEAKING:
                if (static_cast<int32_t>(now_ms - last_progress_ms_) >= static_cast<int32_t>(deadlines.speaking_stall_timeout_ms))
                    return TimeoutReason::SPEAKING_STALLED;
                if (static_cast<int32_t>(now_ms - phase_started_ms_) >= static_cast<int32_t>(deadlines.speaking_max_total_ms))
                    return TimeoutReason::SPEAKING_MAX_EXCEEDED;
                break;
            case SessionPhase::CLEANUP:
                if (static_cast<int32_t>(now_ms - phase_started_ms_) >= static_cast<int32_t>(deadlines.cleanup_timeout_ms))
                    return TimeoutReason::CLEANUP_TIMEOUT;
                break;
            default:
                break;
        }
        return TimeoutReason::NONE;
    }

    static const char *timeout_reason_string(TimeoutReason reason)
    {
        switch (reason)
        {
            case TimeoutReason::CONNECT_TIMEOUT: return "Quá thời gian kết nối/xác thực WSS Xiaozhi";
            case TimeoutReason::LISTENING_TIMEOUT: return "Quá thời gian thu âm giọng nói";
            case TimeoutReason::FLUSH_TIMEOUT: return "Quá thời gian gửi dữ liệu âm thanh";
            case TimeoutReason::WAIT_RESPONSE_TIMEOUT: return "Máy chủ Xiaozhi không phản hồi câu trả lời";
            case TimeoutReason::SPEAKING_STALLED: return "Phát âm thanh Xiaozhi bị gián đoạn (mất luồng)";
            case TimeoutReason::SPEAKING_MAX_EXCEEDED: return "Thời lượng phát câu trả lời vượt mức tối đa";
            case TimeoutReason::CLEANUP_TIMEOUT: return "Quá thời gian giải phóng tài nguyên";
            case TimeoutReason::ABSOLUTE_SESSION_TIMEOUT: return "Phiên Xiaozhi vượt quá giới hạn tối đa";
            default: return "Phiên kết thúc bình thường";
        }
    }

private:
    SessionPhase phase_ = SessionPhase::IDLE;
    uint32_t session_started_ms_ = 0;
    uint32_t phase_started_ms_ = 0;
    uint32_t last_progress_ms_ = 0;
};

inline bool session_id_matches_contract(const char *incoming_session,
                                        const char *expected_session,
                                        bool socket_authenticated)
{
    if (!socket_authenticated) return false;
    if (!incoming_session || !*incoming_session)
    {
        return true;
    }
    return expected_session && strcmp(incoming_session, expected_session) == 0;
}

inline bool has_captured_audio(size_t live_offset, size_t fill, size_t frames_sent)
{
    return live_offset > 0 || fill > 0 || frames_sent > 0;
}

inline bool can_reuse_connection(bool connected, bool clean_turn_end,
                                 size_t uplink_pending, size_t downlink_pending,
                                 uint32_t idle_since_ms, uint32_t now_ms,
                                 uint32_t max_idle_ms)
{
    if (!connected || !clean_turn_end || uplink_pending > 0 || downlink_pending > 0)
        return false;
    return static_cast<int32_t>(now_ms - idle_since_ms) < static_cast<int32_t>(max_idle_ms);
}
}
