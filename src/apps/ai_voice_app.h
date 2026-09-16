/**
 * @file ai_voice_app.h
 * @brief Giao diện ứng dụng AI Voice Assistant trên Mini OS
 * Bong bóng chat Messenger/iMessage, nút tròn Push-to-Talk và sóng âm Waveform động
 */

#pragma once

#include <lvgl.h>

/**
 * @brief Khởi tạo và hiển thị ứng dụng AI Voice Assistant
 * @param parent Container chứa giao diện (kích thước dynamic)
 */
void ai_voice_app_open(lv_obj_t *parent);

/**
 * @brief Đóng và giải phóng tài nguyên ứng dụng AI Voice Assistant
 */
void ai_voice_app_close(void);

/**
 * @brief Cập nhật định kỳ tin nhắn mới, hiệu ứng sóng âm và trạng thái AI (gọi mỗi 30-50ms)
 */
void ai_voice_app_update(void);
