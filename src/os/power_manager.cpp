/**
 * @file power_manager.cpp
 * @brief Triển khai máy trạng thái quản lý nguồn, Inactivity Timer và Touch to Wake cho ESP32-S3
 */

#include "power_manager.h"
#include "../display/lvgl_port.h"

// Biến trạng thái nội bộ
static volatile PowerState current_power_state = POWER_STATE_ACTIVE;
static volatile uint32_t last_activity_millis = 0;
static uint32_t timeout_dim_sec = POWER_TIMEOUT_DIM_DEFAULT_SEC;
static uint32_t timeout_sleep_sec = POWER_TIMEOUT_SLEEP_DEFAULT_SEC;
static uint8_t user_active_brightness = POWER_BRIGHTNESS_ACTIVE_DEFAULT;
static volatile bool touch_wake_suppressed = false;
static volatile bool rendering_paused = false;

void power_manager_init(void)
{
    last_activity_millis = millis();
    current_power_state = POWER_STATE_ACTIVE;
    rendering_paused = false;
    touch_wake_suppressed = false;
    user_active_brightness = POWER_BRIGHTNESS_ACTIVE_DEFAULT;

    // Thiết lập độ sáng ban đầu 100%
    lvgl_port_set_brightness(user_active_brightness);

    Serial.println("[POWER] ✔ Power Manager khởi tạo thành công: Inactivity Timer 60s (Dim 20%) -> 120s (Sleep 0%)");
}

void power_manager_feed_activity(void)
{
    // Nếu màn hình đang không ở trạng thái Active, kích hoạt đánh thức
    if (current_power_state != POWER_STATE_ACTIVE)
    {
        power_manager_wake();
    }
    else
    {
        last_activity_millis = millis();
    }
}

void power_manager_wake(void)
{
    if (current_power_state != POWER_STATE_ACTIVE)
    {
        Serial.printf("[POWER] ⚡ TOUCH TO WAKE: Đánh thức từ trạng thái %s -> ACTIVE (100%%)\n",
            (current_power_state == POWER_STATE_DIMMED) ? "DIMMED (20%)" : "SLEEP (0%)");
        
        // Bật cờ chặn sự kiện click để ngón tay chạm màn hình không vô tình bấm trúng nút bên dưới
        touch_wake_suppressed = true;
    }

    current_power_state = POWER_STATE_ACTIVE;
    rendering_paused = false;
    last_activity_millis = millis();

    // Khôi phục độ sáng 100% ngay lập tức
    lvgl_port_set_brightness(user_active_brightness);
}

void power_manager_sleep(void)
{
    current_power_state = POWER_STATE_DISPLAY_SLEEP;
    lvgl_port_set_brightness(POWER_BRIGHTNESS_SLEEP_DEFAULT);
    rendering_paused = true;
    Serial.println("[POWER] 💤 Buộc chuyển sang chế độ Sleep thủ công");
}

void power_manager_update(void)
{
    uint32_t now = millis();
    uint32_t inactive_ms = now - last_activity_millis;

    // Trạng thái 1: ACTIVE -> Chuyển sang DIMMED sau 60 giây không chạm
    if (current_power_state == POWER_STATE_ACTIVE && inactive_ms >= (timeout_dim_sec * 1000))
    {
        current_power_state = POWER_STATE_DIMMED;
        lvgl_port_set_brightness(POWER_BRIGHTNESS_DIM_DEFAULT);
        Serial.printf("[POWER] 🌙 Không hoạt động > %u giây: Chế độ Dimming (Đèn nền hạ xuống 20%%)\n", timeout_dim_sec);
    }
    // Trạng thái 2: DIMMED (hoặc ACTIVE) -> Chuyển sang SLEEP sau 120 giây không chạm
    else if ((current_power_state == POWER_STATE_DIMMED || current_power_state == POWER_STATE_ACTIVE) && 
             inactive_ms >= (timeout_sleep_sec * 1000))
    {
        current_power_state = POWER_STATE_DISPLAY_SLEEP;
        lvgl_port_set_brightness(POWER_BRIGHTNESS_SLEEP_DEFAULT);
        rendering_paused = true;
        Serial.printf("[POWER] 💤 Không hoạt động > %u giây: Chế độ Display Sleep (Tắt đèn nền 0%% & Tạm dừng render LVGL)\n", timeout_sleep_sec);
    }
}

PowerState power_manager_get_state(void)
{
    return current_power_state;
}

bool power_manager_is_rendering_paused(void)
{
    return rendering_paused;
}

uint32_t power_manager_get_inactivity_seconds(void)
{
    return (millis() - last_activity_millis) / 1000;
}

void power_manager_set_timeouts(uint32_t dim_sec, uint32_t sleep_sec)
{
    if (dim_sec < 10 || sleep_sec <= dim_sec) return;
    timeout_dim_sec = dim_sec;
    timeout_sleep_sec = sleep_sec;
    Serial.printf("[POWER] Đã cập nhật ngưỡng thời gian: Dim %u s | Sleep %u s\n", dim_sec, sleep_sec);
}

uint32_t power_manager_get_dim_timeout(void)
{
    return timeout_dim_sec;
}

uint32_t power_manager_get_sleep_timeout(void)
{
    return timeout_sleep_sec;
}

uint8_t power_manager_get_active_brightness(void)
{
    return user_active_brightness;
}

void power_manager_set_active_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (percent < 10)  percent = 10;
    user_active_brightness = percent;

    if (current_power_state == POWER_STATE_ACTIVE)
    {
        lvgl_port_set_brightness(user_active_brightness);
    }
}

bool power_manager_should_suppress_touch(void)
{
    return touch_wake_suppressed;
}

void power_manager_clear_touch_suppression(void)
{
    touch_wake_suppressed = false;
}
