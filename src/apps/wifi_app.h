/**
 * @file wifi_app.h
 * @brief Module WiFi Settings App (LVGL 8)
 * Danh sách quét mạng, bàn phím ảo cảm ứng và quản lý kết nối
 * Tích hợp Preferences.h NVS Flash lưu trữ thông tin mạng
 */

#pragma once

#include <lvgl.h>

/**
 * @brief Khởi tạo module WiFi App
 */
void wifi_app_init(void);

/**
 * @brief Mở và hiển thị giao diện WiFi Settings App
 * @param parent Khung chứa ứng dụng (kích thước dynamic)
 */
void wifi_app_open(lv_obj_t *parent);

/**
 * @brief Đóng giao diện WiFi Settings App và giải phóng tài nguyên
 */
void wifi_app_close(void);

/**
 * @brief Cập nhật định kỳ trạng thái quét mạng và tiến trình kết nối (gọi từ background loop)
 */
void wifi_app_update(void);

/**
 * @brief Kiểm tra xem giao diện WiFi Settings App có đang mở hay không
 */
bool wifi_app_is_active(void);
