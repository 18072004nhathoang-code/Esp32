/**
 * @file sd_map_cache.h
 * @brief Quản lý bộ nhớ đệm (Cache) bản đồ trên thẻ nhớ MicroSD 32GB FAT32 cho ESP32-S3
 * Hỗ trợ lưu trữ tạm thời ảnh JPEG từ Google Maps Static API để hiển thị tức thì
 * không cần mạng và không tốn hạn ngạch (quota) API.
 */

#pragma once

#include <Arduino.h>
#include <FS.h>
#include "../storage/storage_manager.h"

// Thư mục lưu trữ bản đồ trên thẻ MicroSD
#define SD_MAPS_DIR "/maps"

/**
 * @brief Khởi tạo giao tiếp thẻ nhớ MicroSD (SPI / FAT32)
 * Tự động tạo thư mục /maps nếu chưa tồn tại.
 * @return true nếu thẻ nhớ sẵn sàng, false nếu không phát hiện thẻ nhớ (hệ thống sẽ chạy trực tiếp qua mạng)
 */
bool sd_map_cache_init(void);

/**
 * @brief Kiểm tra xem thẻ nhớ MicroSD đã được kết nối và mount thành công chưa
 */
bool sd_map_cache_is_available(void);

/**
 * @brief Tạo chuỗi đường dẫn tệp trên thẻ nhớ dựa theo tọa độ, zoom và kiểu bản đồ
 * Định dạng: /maps/{lat}_{lon}_z{zoom}_{type}.jpg
 * Ví dụ: /maps/21.0285_105.8542_z15_roadmap.jpg
 */
void sd_map_cache_get_filename(char *out_path, size_t max_len, double lat, double lon, int zoom, const char *maptype);

/**
 * @brief Kiểm tra xem file ảnh bản đồ tại tọa độ này đã có trong thẻ nhớ chưa
 * @return true nếu file đã tồn tại trong thư mục /maps
 */
bool sd_map_cache_exists(double lat, double lon, int zoom, const char *maptype);

/**
 * @brief Đọc file ảnh JPEG từ thẻ nhớ MicroSD vào bộ đệm PSRAM
 * @param lat Vĩ độ
 * @param lon Kinh độ
 * @param zoom Mức thu phóng
 * @param maptype Kiểu bản đồ ("roadmap" hoặc "satellite")
 * @param out_buf Con trỏ đến bộ đệm PSRAM
 * @param max_size Kích thước tối đa của bộ đệm
 * @return Số byte đã đọc, hoặc -1 nếu thất bại
 */
int sd_map_cache_read(double lat, double lon, int zoom, const char *maptype, uint8_t *out_buf, size_t max_size);

/**
 * @brief Ghi dữ liệu ảnh JPEG vừa tải từ Google Maps Static API vào thẻ nhớ MicroSD
 * @param lat Vĩ độ
 * @param lon Kinh độ
 * @param zoom Mức thu phóng
 * @param maptype Kiểu bản đồ ("roadmap" hoặc "satellite")
 * @param in_buf Dữ liệu ảnh JPEG trong PSRAM
 * @param size Dung lượng ảnh (byte)
 * @return true nếu ghi file thành công
 */
bool sd_map_cache_write(double lat, double lon, int zoom, const char *maptype, const uint8_t *in_buf, size_t size);

/**
 * @brief Lấy dung lượng còn trống trên thẻ nhớ MicroSD (MB)
 */
uint64_t sd_map_cache_get_free_mb(void);
