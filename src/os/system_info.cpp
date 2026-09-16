/**
 * @file system_info.cpp
 * @brief Thu thập dữ liệu phần cứng ESP32-S3 theo thời gian thực
 */

#include "system_info.h"
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_freertos_hooks.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../storage/storage_manager.h"
#include "wifi_manager.h"

static volatile uint32_t s_idle_count[2] = {0, 0};
static uint32_t s_last_idle_count[2] = {0, 0};
static uint32_t s_idle_baseline[2] = {0, 0};
static uint64_t s_last_cpu_sample_us = 0;
static bool s_cpu_hooks_ready = false;

static bool idle_hook_core0(void) { ++s_idle_count[0]; return true; }
static bool idle_hook_core1(void) { ++s_idle_count[1]; return true; }

bool system_info_init(void)
{
    if (s_cpu_hooks_ready) return true;
    esp_err_t core0 = esp_register_freertos_idle_hook_for_cpu(idle_hook_core0, 0);
    esp_err_t core1 = esp_register_freertos_idle_hook_for_cpu(idle_hook_core1, 1);
    s_cpu_hooks_ready = core0 == ESP_OK && core1 == ESP_OK;
    s_last_cpu_sample_us = esp_timer_get_time();
    return s_cpu_hooks_ready;
}

SystemStats system_get_stats(void)
{
    SystemStats stats = {};

    stats.cpu_freq_mhz = getCpuFrequencyMhz();
    stats.task_count = uxTaskGetNumberOfTasks();
    stats.loop_stack_free_words = uxTaskGetStackHighWaterMark(nullptr);

    const uint64_t now_us = esp_timer_get_time();
    if (s_cpu_hooks_ready && s_last_cpu_sample_us != 0 && now_us - s_last_cpu_sample_us >= 250000)
    {
        uint64_t idle_delta = 0;
        uint64_t idle_capacity = 0;
        for (int core = 0; core < 2; ++core)
        {
            const uint32_t current = s_idle_count[core];
            const uint32_t delta = current - s_last_idle_count[core];
            s_last_idle_count[core] = current;
            if (delta > s_idle_baseline[core]) s_idle_baseline[core] = delta;
            idle_delta += delta;
            idle_capacity += s_idle_baseline[core];
        }
        if (idle_capacity > 0)
        {
            uint64_t idle_percent_raw = (idle_delta * 100ULL) / idle_capacity;
            if (idle_percent_raw > 100) idle_percent_raw = 100;
            const uint32_t idle_percent = (uint32_t)idle_percent_raw;
            stats.cpu_usage_percent = 100 - idle_percent;
            stats.cpu_usage_available = true;
        }
        s_last_cpu_sample_us = now_us;
    }
    stats.total_heap = ESP.getHeapSize();
    stats.free_heap = ESP.getFreeHeap();
    stats.heap_usage_percent = (stats.total_heap > 0) ? 
        ((stats.total_heap - stats.free_heap) * 100 / stats.total_heap) : 0;

    stats.total_psram = ESP.getPsramSize();
    stats.free_psram = ESP.getFreePsram();
    stats.used_psram = (stats.total_psram > stats.free_psram) ? (stats.total_psram - stats.free_psram) : 0;
    stats.psram_usage_percent = (stats.total_psram > 0) ? 
        (uint8_t)(((uint64_t)stats.used_psram * 100ULL) / stats.total_psram) : 0;

    stats.core_temp_c = temperatureRead();
    stats.flash_size_mb = ESP.getFlashChipSize() / (1024 * 1024);
    stats.storage_available = storage_is_available();
    if (stats.storage_available && storage_lock(20))
    {
        stats.storage_total_mb = storage_get_total_mb();
        stats.storage_free_mb = storage_get_free_mb();
        storage_unlock();
    }
    stats.wifi_connected = wifi_manager_is_connected();
    stats.wifi_rssi = stats.wifi_connected ? wifi_manager_get_rssi() : 0;

    // Dùng esp_timer_get_time() (64-bit μs) để tránh tràn số sau 49.7 ngày của millis()
    uint64_t total_secs = now_us / 1000000ULL;
    stats.uptime_sec = total_secs;
    uint32_t hours = (uint32_t)(total_secs / 3600);
    uint32_t mins  = (uint32_t)((total_secs % 3600) / 60);
    uint32_t secs  = (uint32_t)(total_secs % 60);
    snprintf(stats.uptime_str, sizeof(stats.uptime_str), "%02u:%02u:%02u", hours, mins, secs);

    return stats;
}

BatteryInfo system_get_battery_info(void)
{
    BatteryInfo info;
    memset(&info, 0, sizeof(info));

#if defined(BOARD_BATTERY_ADC_PIN) && (BOARD_BATTERY_ADC_PIN >= 0)
    info.has_battery = true;
    info.is_calibrated = BOARD_BATTERY_CALIBRATED;

    // Đọc điện áp pin ADC qua analogReadMilliVolts
    uint32_t raw_mv = analogReadMilliVolts(BOARD_BATTERY_ADC_PIN);
    
    // Cầu phân áp: Vbat = Vadc * (R1 + R2) / R2
#if defined(BOARD_BATTERY_DIVIDER_R1) && defined(BOARD_BATTERY_DIVIDER_R2)
    const float divider_r2 = BOARD_BATTERY_DIVIDER_R2;
    float ratio = divider_r2 > 0.0f
        ? (BOARD_BATTERY_DIVIDER_R1 + divider_r2) / divider_r2
        : 1.0f;
#else
    float ratio = 2.0f;
#endif
    info.voltage = ((float)raw_mv * ratio) / 1000.0f;

    // Pin Li-Po 3.7V: 3.2V (0%) đến 4.2V (100%)
    if (info.voltage <= 3.20f)
    {
        info.percentage = 0;
    }
    else if (info.voltage >= 4.20f)
    {
        info.percentage = 100;
    }
    else
    {
        info.percentage = (uint8_t)(((info.voltage - 3.20f) / 1.0f) * 100.0f);
    }

    if (info.is_calibrated)
    {
        snprintf(info.status_str, sizeof(info.status_str), "%u%% (%.2fV)", info.percentage, info.voltage);
    }
    else
    {
        snprintf(info.status_str, sizeof(info.status_str), "Uncalibrated (%.2fV)", info.voltage);
    }
#else
    info.has_battery = false;
    info.is_calibrated = false;
    info.voltage = 0.0f;
    info.percentage = 0;
    snprintf(info.status_str, sizeof(info.status_str), "No Battery");
#endif

    return info;
}
