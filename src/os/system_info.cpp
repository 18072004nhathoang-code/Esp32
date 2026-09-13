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
