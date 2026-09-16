#include "settings_service.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace
{
constexpr const char *kNamespace = "mini_os";
constexpr uint32_t kSchemaVersion = 1;

MiniOsSettings s_settings = {85, 0x00F2FE, 60, 120, true};
SemaphoreHandle_t s_mutex = nullptr;
char s_last_error[80] = "Not initialized";

void set_error(const char *message)
{
    strlcpy(s_last_error, message ? message : "Unknown error", sizeof(s_last_error));
}

bool valid(const MiniOsSettings &value)
{
    return value.brightness >= 10 && value.brightness <= 100 &&
           value.dim_timeout_sec >= 10 && value.dim_timeout_sec <= 3600 &&
           value.sleep_timeout_sec > value.dim_timeout_sec && value.sleep_timeout_sec <= 7200;
}

bool persist_locked()
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, false))
    {
        set_error("Cannot open NVS namespace");
        return false;
    }
    bool ok = prefs.putUInt("schema", kSchemaVersion) == sizeof(uint32_t);
    ok = prefs.putUChar("brightness", s_settings.brightness) == sizeof(uint8_t) && ok;
    ok = prefs.putUInt("accent", s_settings.accent_rgb) == sizeof(uint32_t) && ok;
    ok = prefs.putUInt("dim_sec", s_settings.dim_timeout_sec) == sizeof(uint32_t) && ok;
    ok = prefs.putUInt("sleep_sec", s_settings.sleep_timeout_sec) == sizeof(uint32_t) && ok;
    ok = prefs.putBool("wifi_reconn", s_settings.wifi_auto_reconnect) == sizeof(bool) && ok;
    prefs.end();
    set_error(ok ? "OK" : "NVS write failed");
    return ok;
}

template <typename Mutator>
bool update(Mutator mutator)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        set_error("Settings mutex unavailable");
        return false;
    }
    const MiniOsSettings previous = s_settings;
    mutator(s_settings);
    bool ok = valid(s_settings) && persist_locked();
    if (!ok) s_settings = previous;
    xSemaphoreGive(s_mutex);
    return ok;
}
}

bool settings_service_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
    {
        set_error("Cannot create settings mutex");
        return false;
    }

    Preferences prefs;
    bool has_namespace = prefs.begin(kNamespace, true);
    bool needs_persist = !has_namespace;
    if (has_namespace && prefs.getUInt("schema", 0) == kSchemaVersion)
    {
        MiniOsSettings loaded = {
            prefs.getUChar("brightness", s_settings.brightness),
            prefs.getUInt("accent", s_settings.accent_rgb),
            prefs.getUInt("dim_sec", s_settings.dim_timeout_sec),
            prefs.getUInt("sleep_sec", s_settings.sleep_timeout_sec),
            prefs.getBool("wifi_reconn", s_settings.wifi_auto_reconnect)
        };
        if (valid(loaded)) s_settings = loaded;
        else needs_persist = true;
    }
    else if (has_namespace) needs_persist = true;
    if (has_namespace) prefs.end();
    if (needs_persist && !persist_locked()) return false;
    set_error("OK");
    return true;
}

MiniOsSettings settings_service_get(void)
{
    MiniOsSettings copy = s_settings;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        copy = s_settings;
        xSemaphoreGive(s_mutex);
    }
    return copy;
}

bool settings_service_set_brightness(uint8_t value)
{
    if (value < 10 || value > 100) return false;
    return update([value](MiniOsSettings &s) { s.brightness = value; });
}

bool settings_service_set_accent(uint32_t rgb)
{
    return update([rgb](MiniOsSettings &s) { s.accent_rgb = rgb & 0xFFFFFFU; });
}

bool settings_service_set_power_timeouts(uint32_t dim_sec, uint32_t sleep_sec)
{
    if (dim_sec < 10 || sleep_sec <= dim_sec) return false;
    return update([dim_sec, sleep_sec](MiniOsSettings &s) {
        s.dim_timeout_sec = dim_sec;
        s.sleep_timeout_sec = sleep_sec;
    });
}

bool settings_service_set_wifi_auto_reconnect(bool enabled)
{
    return update([enabled](MiniOsSettings &s) { s.wifi_auto_reconnect = enabled; });
}

const char *settings_service_get_last_error(void)
{
    return s_last_error;
}
