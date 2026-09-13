/**
 * @file audio_app.h
 * @brief Ứng dụng Voice AI & Audio Lab Pro Max (480x266)
 * Trực quan hóa sóng âm thanh Mic, Soundboard Loa, Máy ghi âm PSRAM và Trợ lý XiaoZhi AI
 */

#pragma once

#include <lvgl.h>

/**
 * @brief Khởi tạo và hiển thị ứng dụng Audio Lab
 * @param parent Container chứa giao diện (kích thước 480x266)
 */
void audio_app_open(lv_obj_t *parent);

/**
 * @brief Đóng và giải phóng tài nguyên ứng dụng Audio Lab
 */
void audio_app_close(void);

/**
 * @brief Cập nhật định kỳ giao diện sóng âm thanh và VU Meter (gọi mỗi 30-50ms)
 */
void audio_app_update(void);
