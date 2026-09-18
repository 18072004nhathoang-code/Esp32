#pragma once

#include <stddef.h>
#include <stdint.h>

enum class WifiScanPhase : uint8_t
{
    IDLE = 0,
    QUEUED,
    WAITING_FOR_RADIO,
    RUNNING,
    DONE,
    FAILED,
    CANCELED
};

struct WifiScanCoordinator
{
    uint32_t request_id = 0;
    uint32_t revision = 0;
    uint32_t result_revision = 0;
    uint32_t queued_at_ms = 0;
    uint32_t started_at_ms = 0;
    uint32_t deadline_ms = 0;
    WifiScanPhase phase = WifiScanPhase::IDLE;

    static bool terminal(WifiScanPhase value)
    {
        return value == WifiScanPhase::DONE || value == WifiScanPhase::FAILED ||
               value == WifiScanPhase::CANCELED;
    }

    bool busy() const
    {
        return phase == WifiScanPhase::QUEUED || phase == WifiScanPhase::WAITING_FOR_RADIO ||
               phase == WifiScanPhase::RUNNING;
    }

    uint32_t queue(uint32_t now_ms)
    {
        if (busy()) return 0;
        ++request_id;
        if (request_id == 0) ++request_id;
        queued_at_ms = now_ms;
        started_at_ms = 0;
        deadline_ms = 0;
        phase = WifiScanPhase::QUEUED;
        ++revision;
        return request_id;
    }

    bool worker_received(uint32_t id)
    {
        if (id != request_id || phase != WifiScanPhase::QUEUED) return false;
        phase = WifiScanPhase::WAITING_FOR_RADIO;
        ++revision;
        return true;
    }

    bool driver_accepted(uint32_t id, uint32_t now_ms, uint32_t timeout_ms)
    {
        if (id != request_id || phase != WifiScanPhase::WAITING_FOR_RADIO) return false;
        started_at_ms = now_ms;
        deadline_ms = now_ms + timeout_ms;
        phase = WifiScanPhase::RUNNING;
        ++revision;
        return true;
    }

    bool finish(uint32_t id, WifiScanPhase terminal_phase)
    {
        if (id != request_id || !terminal(terminal_phase) || terminal(phase)) return false;
        phase = terminal_phase;
        ++revision;
        ++result_revision;
        return true;
    }

    bool expired(uint32_t id, uint32_t now_ms) const
    {
        return id == request_id && phase == WifiScanPhase::RUNNING && deadline_ms != 0 &&
               static_cast<int32_t>(now_ms - deadline_ms) >= 0;
    }
};

inline bool wifi_scan_result_belongs_to(uint32_t driver_request_id,
                                        const WifiScanCoordinator &coordinator)
{
    return driver_request_id != 0 && driver_request_id == coordinator.request_id &&
           coordinator.phase == WifiScanPhase::RUNNING;
}

inline bool wifi_scan_may_start(bool connection_in_progress, uint32_t now_ms,
                                uint32_t queued_at_ms, uint32_t max_connection_wait_ms)
{
    return !connection_in_progress ||
           static_cast<uint32_t>(now_ms - queued_at_ms) >= max_connection_wait_ms;
}

