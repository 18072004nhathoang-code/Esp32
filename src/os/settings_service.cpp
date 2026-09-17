#include "settings_service.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace
{
constexpr const char *kNamespace = "mini_os";
constexpr uint32_t kSchemaVersion = 2;
constexpr uint32_t kRecordMagic = 0x534F5345U; // "ESOS"

struct __attribute__((packed)) SettingsRecord
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t brightness;
    uint8_t wifi_auto_reconnect;
    uint16_t reserved;
    uint32_t accent_rgb;
    uint32_t dim_timeout_sec;
    uint32_t sleep_timeout_sec;
    uint32_t checksum;
};

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

uint32_t checksum_record(const SettingsRecord &record)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < offsetof(SettingsRecord, checksum); ++i)
        hash = (hash ^ bytes[i]) * 16777619UL;
    return hash;
}

SettingsRecord make_record(const MiniOsSettings &value)
{
    SettingsRecord record = {};
    record.magic = kRecordMagic;
    record.version = kSchemaVersion;
    record.size = sizeof(record);
    record.brightness = value.brightness;
    record.wifi_auto_reconnect = value.wifi_auto_reconnect ? 1 : 0;
    record.accent_rgb = value.accent_rgb;
    record.dim_timeout_sec = value.dim_timeout_sec;
    record.sleep_timeout_sec = value.sleep_timeout_sec;
    record.checksum = checksum_record(record);
    return record;
}

bool decode_record(const SettingsRecord &record, MiniOsSettings &value)
{
    if (record.magic != kRecordMagic || record.version != kSchemaVersion ||
        record.size != sizeof(record) || record.checksum != checksum_record(record) ||
        record.wifi_auto_reconnect > 1) return false;
    value = {record.brightness, record.accent_rgb, record.dim_timeout_sec,
             record.sleep_timeout_sec, record.wifi_auto_reconnect != 0};
    return valid(value);
}

bool persist_locked(const MiniOsSettings &value)
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, false))
    {
        set_error("Cannot open NVS namespace");
        return false;
    }
    const SettingsRecord record = make_record(value);
    const bool ok = prefs.putBytes("record", &record, sizeof(record)) == sizeof(record);
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
    MiniOsSettings candidate = s_settings;
    mutator(candidate);
    const bool ok = valid(candidate) && persist_locked(candidate);
    if (ok) s_settings = candidate;
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
    if (has_namespace && prefs.getBytesLength("record") == sizeof(SettingsRecord))
    {
        SettingsRecord record = {};
        MiniOsSettings loaded = s_settings;
        const bool read_ok = prefs.getBytes("record", &record, sizeof(record)) == sizeof(record);
        if (read_ok && decode_record(record, loaded)) s_settings = loaded;
        else needs_persist = true;
    }
    else if (has_namespace)
    {
        // One-time migration from schema 1's independent keys.
        MiniOsSettings migrated = {
            prefs.getUChar("brightness", s_settings.brightness),
            prefs.getUInt("accent", s_settings.accent_rgb),
            prefs.getUInt("dim_sec", s_settings.dim_timeout_sec),
            prefs.getUInt("sleep_sec", s_settings.sleep_timeout_sec),
            prefs.getBool("wifi_reconn", s_settings.wifi_auto_reconnect)
        };
        if (valid(migrated)) s_settings = migrated;
        needs_persist = true;
    }
    if (has_namespace) prefs.end();
    if (needs_persist && !persist_locked(s_settings)) return false;
    set_error("OK");
    return true;
}

MiniOsSettings settings_service_get(void)
{
    const MiniOsSettings defaults = {85, 0x00F2FE, 60, 120, true};
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return defaults;
    const MiniOsSettings copy = s_settings;
    xSemaphoreGive(s_mutex);
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
