#pragma once

#include <stdint.h>

struct TouchTransformConfig
{
    uint16_t native_width;
    uint16_t native_height;
    uint16_t logical_width;
    uint16_t logical_height;
    uint8_t rotation;
    bool swap_xy;
    bool invert_x;
    bool invert_y;
};

inline bool touch_transform_point(const TouchTransformConfig &cfg,
                                  uint16_t raw_x, uint16_t raw_y,
                                  uint16_t *screen_x, uint16_t *screen_y)
{
    if (!screen_x || !screen_y || cfg.native_width == 0 || cfg.native_height == 0)
        return false;

    // Validate the controller coordinate system before any clamp or transform.
    if (raw_x >= cfg.native_width || raw_y >= cfg.native_height)
        return false;

    int32_t x = raw_x;
    int32_t y = raw_y;
    int32_t width = cfg.native_width;
    int32_t height = cfg.native_height;

    if (cfg.swap_xy)
    {
        const int32_t old_x = x;
        x = y;
        y = old_x;
        const int32_t old_width = width;
        width = height;
        height = old_width;
    }
    if (cfg.invert_x) x = width - 1 - x;
    if (cfg.invert_y) y = height - 1 - y;

    int32_t mapped_x = 0;
    int32_t mapped_y = 0;
    switch (cfg.rotation & 3U)
    {
        case 0: mapped_x = x;              mapped_y = y;              break;
        case 1: mapped_x = y;              mapped_y = width - 1 - x;  break;
        case 2: mapped_x = width - 1 - x;  mapped_y = height - 1 - y; break;
        case 3: mapped_x = height - 1 - y; mapped_y = x;              break;
    }

    if (mapped_x < 0 || mapped_y < 0 ||
        mapped_x >= cfg.logical_width || mapped_y >= cfg.logical_height)
        return false;

    *screen_x = static_cast<uint16_t>(mapped_x);
    *screen_y = static_cast<uint16_t>(mapped_y);
    return true;
}
