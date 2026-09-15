/**
 * @file camera_app.h
 * @brief Giao diện ứng dụng Camera & RTSP Streamer: Cấu hình IP Camera và Preview thời gian thực
 */

#pragma once

#include <lvgl.h>

/**
 * @brief Mở ứng dụng Camera trong container giao diện
 * @param parent Container cha từ UI Manager
 */
void camera_app_open(lv_obj_t *parent);

/**
 * @brief Đóng ứng dụng Camera và giải phóng tài nguyên preview
 */
void camera_app_close(void);

/**
 * @brief Cập nhật định kỳ frame preview từ Camera Service (chạy trong context UI LVGL)
 */
void camera_app_update(void);
