/**
 * @file power_manager.h
 * @brief Module Quản lý Nguồn & Tiết kiệm Năng lượng (Power Manager) cho ESP32-S3 3.5" IPS
 * Hỗ trợ bộ đếm không hoạt động (Inactivity Timer 60s Dimming, 120s Sleep/Tắt màn hình)
 * và cơ chế Chạm để Đánh thức (Touch to Wake) tức thì không chạm nhầm nút bấm.
 */

#pragma once

#include <Arduino.h>

// Trạng thái nguồn của thiết bị
enum PowerState {
    POWER_STATE_ACTIVE = 0,       // Đang hoạt động: Đèn nền 100%, render LVGL đầy đủ
    POWER_STATE_DIMMED,           // Chế độ mờ (sau 60s): Đèn nền giảm 20% để tiết kiệm điện
    POWER_STATE_DISPLAY_SLEEP,    // Chế độ ngủ màn hình (sau 120s): Đèn nền tắt (0%), tạm dừng render LVGL
                                  // CHÚ Ý: Đây là Display Sleep (chỉ tắt hiển thị). Vi điều khiển ESP32-S3
                                  // vẫn hoạt động và các task FreeRTOS (WiFi, Audio, Background) vẫn chạy bình thường.
    POWER_STATE_SLEEP = POWER_STATE_DISPLAY_SLEEP // Bí danh tương thích ngược
};

// Cấu hình thời gian mặc định (giây)
#define POWER_TIMEOUT_DIM_DEFAULT_SEC   60
#define POWER_TIMEOUT_SLEEP_DEFAULT_SEC 120

// Cấu hình độ sáng đèn nền (%)
#define POWER_BRIGHTNESS_ACTIVE_DEFAULT 100
#define POWER_BRIGHTNESS_DIM_DEFAULT    20
#define POWER_BRIGHTNESS_SLEEP_DEFAULT  0

/**
 * @brief Khởi tạo module quản lý nguồn và nạp cấu hình thời gian
 */
void power_manager_init(void);

/**
 * @brief Báo hiệu người dùng vừa tương tác cảm ứng (Reset bộ đếm thời gian về 0)
 */
void power_manager_feed_activity(void);

/**
 * @brief Cập nhật định kỳ trong vòng lặp chính (kiểm tra timeout và chuyển trạng thái)
 */
void power_manager_update(void);

/**
 * @brief Đánh thức màn hình sáng trở lại ngay lập tức (100%) và phục hồi luồng render
 */
void power_manager_wake(void);

/**
 * @brief Chuyển màn hình sang chế độ ngủ (Sleep) ngay lập tức
 */
void power_manager_sleep(void);

/**
 * @brief Lấy trạng thái nguồn hiện tại
 */
PowerState power_manager_get_state(void);

/**
 * @brief Kiểm tra xem luồng đồ họa LVGL có đang tạm dừng để tiết kiệm điện hay không
 */
bool power_manager_is_rendering_paused(void);

/**
 * @brief Lấy số giây người dùng không chạm vào màn hình
 */
uint32_t power_manager_get_inactivity_seconds(void);

/**
 * @brief Thiết lập ngưỡng thời gian Dimming và Sleep tùy biến (giây)
 */
void power_manager_set_timeouts(uint32_t dim_sec, uint32_t sleep_sec);

/**
 * @brief Lấy mức độ sáng hoạt động đang dùng (%)
 */
uint8_t power_manager_get_active_brightness(void);

/**
 * @brief Thiết lập mức độ sáng hoạt động theo người dùng (%)
 */
void power_manager_set_active_brightness(uint8_t percent);

/**
 * @brief Kiểm tra xem có đang trong giai đoạn chặn sự kiện nhấn phím (sau khi vừa đánh thức) không
 */
bool power_manager_should_suppress_touch(void);

/**
 * @brief Bỏ trạng thái chặn sự kiện nhấn (khi người dùng đã nhấc ngón tay ra khỏi màn hình)
 */
void power_manager_clear_touch_suppression(void);
