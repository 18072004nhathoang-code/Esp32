/**
 * @file ui_manager.h
 * @brief Quản trị giao diện Mini OS Desktop và ứng dụng trên LVGL 8
 */

#pragma once

#include <lvgl.h>
#include "../os/system_info.h"

/**
 * @brief Khởi tạo giao diện Desktop, Status bar và các ứng dụng hệ thống
 */
void ui_init(void);

/**
 * @brief Cập nhật thông số hệ thống lên thanh trạng thái và ứng dụng đang mở
 * @param stats Cấu trúc dữ liệu phần cứng mới nhất
 */
void ui_update_periodic(const SystemStats &stats);

/**
 * @brief Mở màn hình ứng dụng WiFi Settings App (hỗ trợ tự động chuyển màn hình khi khởi động)
 */
void ui_open_wifi_app(void);

/**
 * @brief Mở màn hình ứng dụng Music Player (phát nhạc MP3 từ thẻ nhớ MicroSD)
 */
void ui_open_music_app(void);

/**
 * @brief Mở màn hình ứng dụng AI Voice Assistant (XiaoZhi AI Native)
 */
void ui_open_ai_voice_app(void);
