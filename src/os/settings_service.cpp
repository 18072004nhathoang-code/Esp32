#include "settings_service.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/task.h>

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
QueueHandle_t s_command_queue = nullptr;
TaskHandle_t s_worker_task = nullptr;
uint32_t s_completion_revision = 0;
bool s_last_commit_ok = true;
char s_last_error[80] = "Not initialized";
portMUX_TYPE s_error_mux = portMUX_INITIALIZER_UNLOCKED;

enum SettingsCommandType : uint8_t
{
    SETTINGS_SET_BRIGHTNESS = 1,
    SETTINGS_SET_ACCENT,
    SETTINGS_SET_POWER_TIMEOUTS,
    SETTINGS_SET_WIFI_RECONNECT
};

struct SettingsCommand
{
    SettingsCommandType type;
    uint32_t value1;
    uint32_t value2;
};

void set_error(const char *message)
{
    portENTER_CRITICAL(&s_error_mux);
    strlcpy(s_last_error, message ? message : "Unknown error", sizeof(s_last_error));
    portEXIT_CRITICAL(&s_error_mux);
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

bool persist_value(const MiniOsSettings &value)
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
bool update_sync(Mutator mutator)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(250)) != pdTRUE)
    {
        set_error("Settings mutex unavailable");
        return false;
    }
    MiniOsSettings candidate = s_settings;
    xSemaphoreGive(s_mutex);
    mutator(candidate);
    if (!valid(candidate))
    {
        set_error("Invalid settings value");
        return false;
    }
    const bool ok = persist_value(candidate);
    if (ok && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
    {
        s_settings = candidate;
        xSemaphoreGive(s_mutex);
    }
    return ok;
}

void settings_worker(void *)
{
    SettingsCommand command = {};
    for (;;)
    {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        bool ok = false;
        switch (command.type)
        {
            case SETTINGS_SET_BRIGHTNESS:
                ok = update_sync([&command](MiniOsSettings &s) {
                    s.brightness = static_cast<uint8_t>(command.value1);
                });
                break;
            case SETTINGS_SET_ACCENT:
                ok = update_sync([&command](MiniOsSettings &s) {
                    s.accent_rgb = command.value1 & 0xFFFFFFU;
                });
                break;
            case SETTINGS_SET_POWER_TIMEOUTS:
                ok = update_sync([&command](MiniOsSettings &s) {
                    s.dim_timeout_sec = command.value1;
                    s.sleep_timeout_sec = command.value2;
                });
                break;
            case SETTINGS_SET_WIFI_RECONNECT:
                ok = update_sync([&command](MiniOsSettings &s) {
                    s.wifi_auto_reconnect = command.value1 != 0;
                });
                break;
            default:
                set_error("Unknown settings command");
                break;
        }
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE)
        {
            s_last_commit_ok = ok;
            ++s_completion_revision;
            if (s_completion_revision == 0) ++s_completion_revision;
            xSemaphoreGive(s_mutex);
        }
    }
}

bool enqueue_command(const SettingsCommand &command)
{
    if (s_worker_task && s_command_queue &&
        xQueueSend(s_command_queue, &command, 0) == pdTRUE) return true;
    set_error("Settings command queue full");
    return false;
}
}

bool settings_service_init(void)
{
    if (s_worker_task) return true;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_command_queue) s_command_queue = xQueueCreate(8, sizeof(SettingsCommand));
    if (!s_mutex || !s_command_queue)
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
    if (needs_persist && !persist_value(s_settings)) return false;
    if (!s_worker_task && xTaskCreatePinnedToCore(settings_worker, "SettingsWorker", 4096,
                                                  nullptr, 1, &s_worker_task, 0) != pdPASS)
    {
        s_worker_task = nullptr;
        set_error("Cannot create settings worker");
        return false;
    }
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
    return enqueue_command({SETTINGS_SET_BRIGHTNESS, value, 0});
}

bool settings_service_set_accent(uint32_t rgb)
{
    return enqueue_command({SETTINGS_SET_ACCENT, rgb & 0xFFFFFFU, 0});
}

bool settings_service_set_power_timeouts(uint32_t dim_sec, uint32_t sleep_sec)
{
    if (dim_sec < 10 || dim_sec > 3600 || sleep_sec <= dim_sec || sleep_sec > 7200)
        return false;
    return enqueue_command({SETTINGS_SET_POWER_TIMEOUTS, dim_sec, sleep_sec});
}

bool settings_service_set_wifi_auto_reconnect(bool enabled)
{
    return enqueue_command({SETTINGS_SET_WIFI_RECONNECT, enabled ? 1U : 0U, 0});
}

const char *settings_service_get_last_error(void)
{
    return s_last_error;
}

void settings_service_copy_last_error(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    portENTER_CRITICAL(&s_error_mux);
    strlcpy(out, s_last_error, out_size);
    portEXIT_CRITICAL(&s_error_mux);
}

uint32_t settings_service_get_completion_revision(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    const uint32_t revision = s_completion_revision;
    xSemaphoreGive(s_mutex);
    return revision;
}

bool settings_service_last_commit_ok(void)
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    const bool ok = s_last_commit_ok;
    xSemaphoreGive(s_mutex);
    return ok;
}
