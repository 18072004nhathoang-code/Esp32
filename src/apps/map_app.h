/**
 * @file map_app.h
 * @brief Ứng dụng xem bản đồ Google Maps tương tác trên Mini OS cho ESP32-S3
 * Hỗ trợ Google Maps Static API (480x320, solution_id=gmp_git_agentskills_v1),
 * bộ nhớ đệm MicroSD FAT32 Cache, chuyển đổi Roadmap/Satellite và giao diện cảm ứng Zoom/Pan.
 */

#pragma once

#include <Arduino.h>
#include <lvgl.h>

#include "../display/lvgl_port.h"

// Kích thước khung hình bản đồ tương thích màn hình 240x320 Portrait
#ifndef MAP_CANVAS_WIDTH
#define MAP_CANVAS_WIDTH  240
#endif
#ifndef MAP_CANVAS_HEIGHT
#define MAP_CANVAS_HEIGHT 270 // Chiếm toàn bộ vùng nội dung ứng dụng
#endif

// Tọa độ mặc định: Hà Nội (Hồ Hoàn Kiếm)
#define MAP_DEFAULT_LAT   21.0285
#define MAP_DEFAULT_LON   105.8542
#define MAP_DEFAULT_ZOOM  15

struct MapPresetLocation {
    const char *name;
    double lat;
    double lon;
    uint8_t zoom;
};

/**
 * @brief Mở ứng dụng Google Maps bên trong khung chứa cha
 * @param parent Đối tượng container chứa nội dung ứng dụng
 */
void map_app_open(lv_obj_t *parent);

/**
 * @brief Đóng ứng dụng Google Maps và giải phóng bộ nhớ Canvas
 */
void map_app_close(void);

/**
 * @brief Cập nhật tọa độ và vẽ lại bản đồ
 */
void map_app_render(void);

/**
 * @brief Thu phóng bản đồ (Zoom In)
 */
void map_app_zoom_in(void);

/**
 * @brief Thu nhỏ bản đồ (Zoom Out)
 */
void map_app_zoom_out(void);

/**
 * @brief Dịch chuyển bản đồ theo các hướng (Pan)
 * @param dir 1: Lên (Bắc), 2: Xuống (Nam), 3: Trái (Tây), 4: Phải (Đông)
 */
void map_app_pan_direction(uint8_t dir);

/**
 * @brief Chuyển đổi giữa chế độ Bản đồ đường phố (roadmap) và Ảnh vệ tinh (satellite)
 */
void map_app_toggle_map_type(void);

/**
 * @brief Lấy kiểu bản đồ hiện tại ("roadmap" hoặc "satellite")
 */
const char* map_app_get_current_type(void);
