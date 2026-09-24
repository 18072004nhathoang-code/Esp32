#include "time_service.h"

#include <Arduino.h>
#include <time.h>

#include "time_service_logic.h"
#include "wifi_manager.h"

namespace
{
TimeSyncLogic g_sync;
constexpr const char *kTimezone = "ICT-7";
portMUX_TYPE g_time_mux = portMUX_INITIALIZER_UNLOCKED;
}

void time_service_init(void)
{
    setenv("TZ", kTimezone, 1);
    tzset();
    const time_t now = time(nullptr);
    portENTER_CRITICAL(&g_time_mux);
    g_sync.observe_epoch(now);
    portEXIT_CRITICAL(&g_time_mux);
}

void time_service_update(void)
{
    const uint32_t now_ms = millis();
    const bool connected = wifi_manager_is_connected();
    bool request_sync = false;
    portENTER_CRITICAL(&g_time_mux);
    request_sync = g_sync.should_request(connected, now_ms);
    if (request_sync) g_sync.requested(now_ms);
    portEXIT_CRITICAL(&g_time_mux);
    if (request_sync)
    {
        configTzTime(kTimezone, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        Serial.println("[TIME] SNTP sync requested (UTC+7)");
    }

    const time_t epoch = time(nullptr);
    bool became_synced = false;
    portENTER_CRITICAL(&g_time_mux);
    const bool before = g_sync.synced;
    g_sync.observe_epoch(epoch);
    became_synced = !before && g_sync.synced;
    portEXIT_CRITICAL(&g_time_mux);
    if (became_synced) Serial.println("[TIME] SNTP synchronized");
}

bool time_service_is_synced(void)
{
    const time_t now = time(nullptr);
    portENTER_CRITICAL(&g_time_mux);
    g_sync.observe_epoch(now);
    const bool synced = g_sync.synced;
    portEXIT_CRITICAL(&g_time_mux);
    return synced;
}

bool time_service_format_clock(char *buffer, size_t size)
{
    if (!buffer || size < 6) return false;
    const time_t now = time(nullptr);
    portENTER_CRITICAL(&g_time_mux);
    g_sync.observe_epoch(now);
    const bool synced = g_sync.synced;
    portEXIT_CRITICAL(&g_time_mux);
    if (!synced)
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
