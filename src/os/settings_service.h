#pragma once

#include <Arduino.h>

struct MiniOsSettings
{
    uint8_t brightness;
    uint32_t accent_rgb;
    uint32_t dim_timeout_sec;
    uint32_t sleep_timeout_sec;
    bool wifi_auto_reconnect;
};

bool settings_service_init(void);
MiniOsSettings settings_service_get(void);
bool settings_service_set_brightness(uint8_t value);
bool settings_service_set_accent(uint32_t rgb);
bool settings_service_set_power_timeouts(uint32_t dim_sec, uint32_t sleep_sec);
bool settings_service_set_wifi_auto_reconnect(bool enabled);
const char *settings_service_get_last_error(void);
void settings_service_copy_last_error(char *out, size_t out_size);
uint32_t settings_service_get_completion_revision(void);
bool settings_service_last_commit_ok(void);
