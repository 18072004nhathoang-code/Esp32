/**
 * @file music_app.h
 * @brief Ứng dụng Music Player Pro Max trên Mini OS
 * Artwork đĩa than xoay, danh sách MP3, thanh tiến trình và điều khiển cảm ứng
 */

#pragma once

#include <lvgl.h>

/**
 * @brief Khởi tạo và hiển thị ứng dụng Music Player
 * @param parent Container chứa giao diện (kích thước dynamic)
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
