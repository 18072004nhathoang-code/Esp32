/**
 * @file wifi_app.h
 * @brief Module WiFi Settings App (LVGL 8 cho màn hình 3.5" IPS 480x320)
 * Giao diện chia 2 nửa: Quét mạng (trái) và Bàn phím ảo cảm ứng / Nhập mật khẩu (phải)
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
 * @param parent Khung chứa ứng dụng (480x266)
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
