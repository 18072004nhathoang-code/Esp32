#pragma once

#include <stdint.h>
#include <stdio.h>
#include <time.h>

struct TimeSyncLogic
{
    bool connected = false;
    bool synced = false;
    uint32_t last_request_ms = 0;

    static bool valid_epoch(time_t epoch)
    {
        return epoch >= 1704067200;
    }

    bool should_request(bool now_connected, uint32_t now_ms,
                        uint32_t retry_ms = 30000,
                        uint32_t resync_ms = 21600000)
    {
        const bool rising_edge = now_connected && !connected;
        connected = now_connected;
        if (!now_connected) return false;
        const uint32_t interval = synced ? resync_ms : retry_ms;
        return rising_edge || last_request_ms == 0 ||
               static_cast<uint32_t>(now_ms - last_request_ms) >= interval;
    }

    void requested(uint32_t now_ms)
    {
        last_request_ms = now_ms ? now_ms : 1;
    }

    void observe_epoch(time_t epoch)
    {
        if (valid_epoch(epoch)) synced = true;
    }
};

inline void utc7_hhmm(time_t epoch, char out[6])
{
    const uint32_t seconds = static_cast<uint32_t>((epoch + 7 * 3600) % 86400);
    snprintf(out, 6, "%02u:%02u", seconds / 3600, (seconds % 3600) / 60);
}

