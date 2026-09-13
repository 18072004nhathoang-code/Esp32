/**
 * @file system_info.h
 * @brief Module giám sát tài nguyên phần cứng Mini OS trên ESP32-S3
 */

#pragma once

#include <Arduino.h>

struct SystemStats {
    uint32_t cpu_freq_mhz;
    uint32_t free_heap;
    uint32_t total_heap;
    uint8_t  heap_usage_percent;
    uint32_t free_psram;
    uint32_t total_psram;
    uint32_t used_psram;
    uint8_t  psram_usage_percent;
    float    core_temp_c;
    uint32_t flash_size_mb;
    uint64_t uptime_sec;
    char     uptime_str[24];
};

/**
 * @brief Thu thập thông số hoạt động của chip ESP32-S3
 */
SystemStats system_get_stats(void);
