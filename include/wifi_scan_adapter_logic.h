#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct WifiScanSettings
{
    bool show_hidden = true;
    uint16_t active_min_ms = 40;
    uint16_t active_max_ms = 120;
    uint8_t home_channel_dwell_ms = 30;
};

template <typename Config>
inline void wifi_scan_zero_config(Config &config)
{
    memset(&config, 0, sizeof(config));
}

enum class WifiScanDriverPhase : uint8_t
{
    IDLE = 0,
    STARTING,
    RUNNING,
    STOPPING,
    DRAINING,
    RECOVERING,
    COMPLETED,
    RECOVERY_FAILED
};

struct WifiScanDriverCompletion
{
    uint32_t request_id = 0;
    uint32_t status = 0;
    uint8_t result_count = 0;
    uint8_t scan_id = 0;
};

struct WifiScanAdapterLogic
{
    WifiScanDriverPhase phase = WifiScanDriverPhase::IDLE;
    uint32_t active_request_id = 0;
    uint32_t phase_started_ms = 0;
    uint32_t stale_event_count = 0;
    bool completion_ready = false;
    bool drained_event_ready = false;
    WifiScanDriverCompletion completion = {};

    bool prepare_start(uint32_t request_id, uint32_t now_ms)
    {
        if (!request_id || phase != WifiScanDriverPhase::IDLE) return false;
        phase = WifiScanDriverPhase::STARTING;
        active_request_id = request_id;
        phase_started_ms = now_ms;
        completion_ready = false;
        drained_event_ready = false;
        completion = {};
        return true;
    }

    bool start_returned(uint32_t request_id, int32_t error,
                        bool driver_may_still_be_busy, uint32_t now_ms)
    {
        const bool early_completion = phase == WifiScanDriverPhase::COMPLETED &&
                                      completion_ready;
        if ((phase != WifiScanDriverPhase::STARTING && !early_completion) ||
            active_request_id != request_id) return false;
        if (error == 0)
        {
            phase = completion_ready ? WifiScanDriverPhase::COMPLETED
                                     : WifiScanDriverPhase::RUNNING;
            return true;
        }

        if (completion_ready) drained_event_ready = true;
        completion_ready = false;
        completion = {};
        active_request_id = 0;
        phase_started_ms = now_ms;
        phase = driver_may_still_be_busy ? WifiScanDriverPhase::DRAINING
                                         : WifiScanDriverPhase::IDLE;
        return false;
    }

    bool on_scan_done(uint8_t scan_id, uint32_t status, uint8_t result_count)
    {
        if ((phase == WifiScanDriverPhase::STARTING ||
             phase == WifiScanDriverPhase::RUNNING) && active_request_id)
        {
            completion.request_id = active_request_id;
            completion.status = status;
            completion.result_count = result_count;
            completion.scan_id = scan_id;
            completion_ready = true;
            phase = WifiScanDriverPhase::COMPLETED;
            return true;
        }
        if (phase == WifiScanDriverPhase::STOPPING ||
            phase == WifiScanDriverPhase::DRAINING ||
            phase == WifiScanDriverPhase::RECOVERING)
        {
            drained_event_ready = true;
            active_request_id = 0;
            phase = WifiScanDriverPhase::IDLE;
            return false;
        }
        ++stale_event_count;
        return false;
    }

    bool begin_stop(uint32_t request_id, uint32_t now_ms)
    {
        if (!request_id || request_id != active_request_id ||
            (phase != WifiScanDriverPhase::STARTING &&
             phase != WifiScanDriverPhase::RUNNING)) return false;
        phase = WifiScanDriverPhase::STOPPING;
        phase_started_ms = now_ms;
        completion_ready = false;
        completion = {};
        return true;
    }

    void stop_returned(uint32_t request_id, bool driver_may_still_be_busy,
                       uint32_t now_ms)
    {
        if (phase != WifiScanDriverPhase::STOPPING ||
            active_request_id != request_id) return;
        phase_started_ms = now_ms;
        if (driver_may_still_be_busy)
        {
            phase = WifiScanDriverPhase::DRAINING;
        }
        else
        {
            active_request_id = 0;
            phase = WifiScanDriverPhase::IDLE;
            drained_event_ready = true;
        }
    }

    bool take_completion(uint32_t request_id, WifiScanDriverCompletion &out)
    {
        if (!completion_ready || phase != WifiScanDriverPhase::COMPLETED ||
            completion.request_id != request_id) return false;
        out = completion;
        completion_ready = false;
        return true;
    }

    bool release_completion(uint32_t request_id)
    {
        if (phase != WifiScanDriverPhase::COMPLETED ||
            active_request_id != request_id) return false;
        active_request_id = 0;
        completion = {};
        completion_ready = false;
        phase = WifiScanDriverPhase::IDLE;
        return true;
    }

    bool take_drained_event()
    {
        const bool ready = drained_event_ready;
        drained_event_ready = false;
        return ready;
    }

    bool drain_expired(uint32_t now_ms, uint32_t timeout_ms) const
    {
        return phase == WifiScanDriverPhase::DRAINING &&
               static_cast<uint32_t>(now_ms - phase_started_ms) >= timeout_ms;
    }

    static constexpr uint8_t kMaxRecoveryRetries = 3;
    uint8_t recovery_retries = 0;

    bool begin_recovery(uint32_t now_ms)
    {
        if (phase != WifiScanDriverPhase::DRAINING &&
            phase != WifiScanDriverPhase::RECOVERING &&
            phase != WifiScanDriverPhase::RECOVERY_FAILED) return false;
        phase = WifiScanDriverPhase::RECOVERING;
        phase_started_ms = now_ms;
        active_request_id = 0;
        return true;
    }

    void recovery_finished(bool success, uint32_t now_ms = 0)
    {
        if (phase != WifiScanDriverPhase::RECOVERING) return;
        phase_started_ms = now_ms;
        if (success)
        {
            phase = WifiScanDriverPhase::IDLE;
            drained_event_ready = true;
            recovery_retries = 0;
        }
        else
        {
            ++recovery_retries;
            if (recovery_retries >= kMaxRecoveryRetries)
            {
                phase = WifiScanDriverPhase::RECOVERY_FAILED;
                drained_event_ready = false;
            }
            else
            {
                phase = WifiScanDriverPhase::DRAINING;
            }
        }
    }

    bool retry_failed_recovery(uint32_t now_ms)
    {
        if (phase != WifiScanDriverPhase::RECOVERY_FAILED) return false;
        recovery_retries = 0;
        return begin_recovery(now_ms);
    }

};

inline bool wifi_scan_total_expired(uint32_t now_ms, uint32_t queued_at_ms,
                                    uint32_t total_budget_ms)
{
    return static_cast<uint32_t>(now_ms - queued_at_ms) >= total_budget_ms;
}

inline size_t wifi_scan_bounded_result_count(size_t reported, size_t capacity)
{
    return reported < capacity ? reported : capacity;
}
