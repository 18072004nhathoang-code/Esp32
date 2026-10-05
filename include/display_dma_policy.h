#pragma once
#include <stddef.h>
#include <stdint.h>

namespace display_dma {
// Synchronous flush: smaller strips change transfer count, not screen geometry
// or pixel format. 240 * 8 * 2 = 3840 bytes, leaving SRAM for BLE/audio/WiFi.
constexpr uint16_t kPortraitLines = 8;
constexpr size_t bytes(uint16_t width, uint16_t lines, size_t pixel_bytes) {
    return static_cast<size_t>(width) * lines * pixel_bytes;
}
}
