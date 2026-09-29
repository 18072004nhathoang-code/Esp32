#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t PHONE_MAP_MAX_JPEG_BYTES = 128U * 1024U;
constexpr uint16_t PHONE_MAP_HTTP_PORT = 8080;

inline bool phone_map_generation_accepts(uint32_t uploaded_generation,
                                         uint32_t active_generation,
                                         bool request_active)
{
    return request_active && uploaded_generation != 0 &&
           uploaded_generation == active_generation;
}

inline bool phone_map_jpeg_envelope_valid(const uint8_t *data, size_t size)
{
    return data && size > 200U && size <= PHONE_MAP_MAX_JPEG_BYTES &&
           data[0] == 0xFF && data[1] == 0xD8 &&
           data[size - 2] == 0xFF && data[size - 1] == 0xD9;
}
