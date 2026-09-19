#pragma once

#include <stddef.h>
#include <stdint.h>

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
}
