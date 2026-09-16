/**
 * @file system_info.h
 * @brief Module giám sát tài nguyên phần cứng Mini OS trên ESP32-S3
 */

#pragma once

#include <Arduino.h>

struct SystemStats {
    uint32_t cpu_freq_mhz;
    uint8_t  cpu_usage_percent;
    bool     cpu_usage_available;
    uint32_t task_count;
    uint32_t loop_stack_free_words;
    uint32_t free_heap;
    uint32_t total_heap;
    uint8_t  heap_usage_percent;
    uint32_t free_psram;
    uint32_t total_psram;
    uint32_t used_psram;
    uint8_t  psram_usage_percent;
    float    core_temp_c;
    uint32_t flash_size_mb;
    bool     storage_available;
    uint64_t storage_total_mb;
    uint64_t storage_free_mb;
    bool     wifi_connected;
    int8_t   wifi_rssi;
    uint64_t uptime_sec;
    char     uptime_str[24];
};

struct BatteryInfo {
    bool has_battery;
    bool is_calibrated;
    float voltage;
    uint8_t percentage;
    char status_str[40];
};

/**
 * @brief Thu thập thông số hoạt động của chip ESP32-S3
 */
bool system_info_init(void);
SystemStats system_get_stats(void);

/**
 * @brief Thu thập thông số pin phần cứng thực tế
 */
BatteryInfo system_get_battery_info(void);
