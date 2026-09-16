/**
 * @file map_tile_downloader.h
 * @brief Module tải ảnh bản đồ thực tế từ Google Maps Static API / OpenStreetMap
 * và tích hợp bộ nhớ đệm MicroSD FAT32 Cache, giải mã JPEG bằng TJpgDec trong 8MB PSRAM.
 */

#pragma once

#include <Arduino.h>
#include <lvgl.h>

#include "../display/lvgl_port.h"

// Kích thước chuẩn theo logical display hiện tại.
#ifndef MAP_TILE_WIDTH
#define MAP_TILE_WIDTH  DISP_HOR_RES
#endif
#ifndef MAP_TILE_HEIGHT
#define MAP_TILE_HEIGHT DISP_VER_RES
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// Cấu hình Google Maps Static API Key
// Người dùng có thể truyền trực tiếp hoặc để trống để sử dụng Maps Demo Key / OpenStreetMap Fallback
#ifndef GOOGLE_MAPS_STATIC_API_KEY
#define GOOGLE_MAPS_STATIC_API_KEY ""
#endif

// Mã định danh bắt buộc tuân thủ quy chuẩn Google Maps Platform
#define GMP_SOLUTION_ID "gmp_git_agentskills_v1"

// Trạng thái của quá trình tải ảnh
enum TileDownloadStatus {
    TILE_IDLE = 0,
    TILE_DOWNLOADING,
    TILE_READY,
    TILE_ERROR,
    TILE_DEGRADED
};

// Nguồn cung cấp ảnh bản đồ hiện tại
enum TileSource {
    TILE_SOURCE_NONE = 0,
    TILE_SOURCE_SD_CACHE,     // Đọc trực tiếp từ thẻ nhớ MicroSD FAT32 (Tức thì, không tốn quota)
    TILE_SOURCE_NETWORK,      // Tải mới từ Google Maps Static API qua WiFi
    TILE_SOURCE_OFFLINE_VECTOR// Chế độ vector nội suy offline
};

/**
 * @brief Khởi tạo bộ đệm trong 8MB Octal PSRAM, thẻ MicroSD và FreeRTOS Task chạy ngầm trên Core 0
 */
bool map_tile_downloader_init(void);

/**
 * @brief Gửi yêu cầu tải hoặc nạp ảnh bản đồ cho tọa độ và mức zoom
 * @param lat Vĩ độ (Latitude)
 * @param lon Kinh độ (Longitude)
 * @param zoom Mức thu phóng (5 đến 20)
 * @param maptype Kiểu bản đồ: "roadmap" (mặc định) hoặc "satellite"
 */
bool map_tile_downloader_request(double lat, double lon, int zoom, const char *maptype = "roadmap");

/**
 * @brief Kiểm tra xem đã có ảnh bản đồ mới giải mã xong chưa
 * @return true nếu có dữ liệu RGB565 mới trong PSRAM
 */
bool map_tile_downloader_has_new_data(void);

/**
 * @brief Lấy con trỏ đến bộ đệm ảnh RGB565 trong PSRAM
 */
const lv_color_t* map_tile_downloader_get_buffer(void);

/**
 * @brief Sao chép an toàn dữ liệu từ Front Buffer sang bộ đệm đích dưới khóa Mutex (chống tearing)
 */
bool map_tile_downloader_copy_front(lv_color_t *dest, size_t count_pixels);

/**
 * @brief Tiêu thụ nguyên tử frame mới: kiểm tra cờ, sao chép dữ liệu, lấy TileSource và xóa cờ trong 1 critical section
 * @param dest Bộ đệm đích để nhận điểm ảnh RGB565
 * @param count_pixels Số điểm ảnh
 * @param out_source Con trỏ nhận nguồn ảnh (SD Cache hoặc Network)
 * @return true nếu có frame mới được tiêu thụ thành công
 */
bool map_tile_downloader_consume_front(lv_color_t *dest, size_t count_pixels, TileSource *out_source = nullptr);

/**
 * @brief Đánh dấu đã nạp xong ảnh vào màn hình
 */
void map_tile_downloader_clear_new_data(void);

/**
 * @brief Lấy trạng thái chi tiết của tiến trình tải ảnh
 */
TileDownloadStatus map_tile_downloader_get_status(void);

/**
 * @brief Lấy nguồn dữ liệu của ảnh bản đồ hiện tại (SD Cache hay Mạng)
 */
TileSource map_tile_downloader_get_source(void);

/**
 * @brief Thiết lập động Google Maps API Key
 */
void map_tile_downloader_set_api_key(const char *key);
