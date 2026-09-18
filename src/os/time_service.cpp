#include "time_service.h"

#include <Arduino.h>
#include <time.h>

#include "time_service_logic.h"
#include "wifi_manager.h"

namespace
{
TimeSyncLogic g_sync;
constexpr const char *kTimezone = "ICT-7";
}

void time_service_init(void)
{
    setenv("TZ", kTimezone, 1);
    tzset();
    g_sync.observe_epoch(time(nullptr));
}

void time_service_update(void)
{
    const uint32_t now_ms = millis();
    const bool connected = wifi_manager_is_connected();
    if (g_sync.should_request(connected, now_ms))
    {
        configTzTime(kTimezone, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        g_sync.requested(now_ms);
        Serial.println("[TIME] SNTP sync requested (UTC+7)");
    }
    const bool before = g_sync.synced;
    g_sync.observe_epoch(time(nullptr));
    if (!before && g_sync.synced) Serial.println("[TIME] SNTP synchronized");
}

bool time_service_is_synced(void)
{
    g_sync.observe_epoch(time(nullptr));
    return g_sync.synced;
}

bool time_service_format_clock(char *buffer, size_t size)
{
    if (!buffer || size < 6) return false;
    const time_t now = time(nullptr);
    g_sync.observe_epoch(now);
    if (!g_sync.synced)
    {
        strlcpy(buffer, "--:--", size);
        return false;
    }
    struct tm local = {};
    if (!localtime_r(&now, &local))
    {
        strlcpy(buffer, "--:--", size);
        return false;
    }
    snprintf(buffer, size, "%02d:%02d", local.tm_hour, local.tm_min);
    return true;
}

