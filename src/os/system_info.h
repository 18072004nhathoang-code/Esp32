/**
 * @file system_info.h
 * @brief Module giám sát tài nguyên phần cứng Mini OS trên ESP32-S3
 */

#pragma once

#include <Arduino.h>
#include "runtime_health.h"

struct SystemStats {
    uint32_t cpu_freq_mhz;
    uint8_t  cpu_usage_percent;
    bool     cpu_usage_available;
    bool     cpu_usage_estimated;
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

struct RuntimeHealthSnapshot {
    uint32_t internal_free_bytes;
    uint32_t internal_min_free_bytes;
    uint32_t internal_largest_free_bytes;
    uint32_t psram_free_bytes;
    uint32_t psram_min_free_bytes;
    uint32_t psram_largest_free_bytes;
    uint32_t lvgl_free_bytes;
    uint32_t lvgl_largest_free_bytes;
    uint32_t loop_stack_free_bytes;
    uint32_t task_count;
    uint32_t reset_reason;
    uint32_t voice_uplink_queue_depth;
    uint32_t voice_inbound_queue_depth;
    uint32_t voice_uplink_drops;
    uint32_t voice_inbound_drops;
    uint8_t lvgl_fragmentation_percent;
    bool lvgl_stats_available;
    RuntimeTaskHealth tasks[RUNTIME_TASK_COUNT];
};

/**
 * @brief Thu thập thông số hoạt động của chip ESP32-S3
 */
bool system_info_init(void);
void system_info_update(void);
SystemStats system_get_stats(void);
RuntimeHealthSnapshot system_get_runtime_health(void);

/**
 * @brief Thu thập thông số pin phần cứng thực tế
 */
BatteryInfo system_get_battery_info(void);
