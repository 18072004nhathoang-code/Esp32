/**
 * @file music_app.h
 * @brief Ứng dụng Music Player Pro Max trên màn hình 3.5" IPS 480x320
 * Giao diện chia đôi: Cột trái danh sách MP3 từ /music, Cột phải đĩa than quay và điều khiển cảm ứng
 */

#pragma once

#include <lvgl.h>

/**
 * @brief Khởi tạo và hiển thị ứng dụng Music Player
 * @param parent Container chứa giao diện (kích thước 480x266)
 */
void music_app_open(lv_obj_t *parent);

/**
 * @brief Đóng và giải phóng tài nguyên ứng dụng Music Player
 */
void music_app_close(void);

/**
 * @brief Cập nhật định kỳ trạng thái phát nhạc, thanh tua và đĩa than (gọi mỗi 50-100ms)
 */
void music_app_update(void);
