/**
 * @file system_info.cpp
 * @brief Thu thập dữ liệu phần cứng ESP32-S3 theo thời gian thực
 */

#include "system_info.h"
#include <esp_system.h>
#include <esp_timer.h>

SystemStats system_get_stats(void)
{
    SystemStats stats;

    stats.cpu_freq_mhz = getCpuFrequencyMhz();
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

    // Dùng esp_timer_get_time() (64-bit μs) để tránh tràn số sau 49.7 ngày của millis()
    uint64_t total_secs = (uint64_t)(esp_timer_get_time() / 1000000ULL);
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
#if defined(BOARD_BATTERY_DIVIDER_R1) && defined(BOARD_BATTERY_DIVIDER_R2) && (BOARD_BATTERY_DIVIDER_R2 > 0)
    float ratio = (BOARD_BATTERY_DIVIDER_R1 + BOARD_BATTERY_DIVIDER_R2) / BOARD_BATTERY_DIVIDER_R2;
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
