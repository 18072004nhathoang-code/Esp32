/**
 * @file power_manager.cpp
 * @brief Triển khai máy trạng thái quản lý nguồn, Inactivity Timer và Touch to Wake cho ESP32-S3
 */

#include "power_manager.h"
#include "../display/lvgl_port.h"
#include "firmware_contracts.h"

// Biến trạng thái nội bộ
static volatile PowerState current_power_state = POWER_STATE_ACTIVE;
static volatile uint32_t last_activity_millis = 0;
static volatile uint32_t activity_revision = 0;
static portMUX_TYPE power_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t timeout_dim_sec = POWER_TIMEOUT_DIM_DEFAULT_SEC;
static uint32_t timeout_sleep_sec = POWER_TIMEOUT_SLEEP_DEFAULT_SEC;
static uint8_t user_active_brightness = POWER_BRIGHTNESS_ACTIVE_DEFAULT;
static volatile bool touch_wake_suppressed = false;
static volatile bool rendering_paused = false;

void power_manager_init(void)
{
    portENTER_CRITICAL(&power_mux);
    last_activity_millis = millis();
    ++activity_revision;
    current_power_state = POWER_STATE_ACTIVE;
    rendering_paused = false;
    touch_wake_suppressed = false;
    portEXIT_CRITICAL(&power_mux);
    user_active_brightness = POWER_BRIGHTNESS_ACTIVE_DEFAULT;

    // Thiết lập độ sáng ban đầu 100%
    lvgl_port_set_brightness(user_active_brightness);

    Serial.println("[POWER] ✔ Power Manager khởi tạo thành công: Inactivity Timer 60s (Dim 20%) -> 120s (Sleep 0%)");
}

void power_manager_feed_activity(void)
{
    portENTER_CRITICAL(&power_mux);
    const PowerState state = current_power_state;
    portEXIT_CRITICAL(&power_mux);

    // Nếu màn hình đang không ở trạng thái Active, kích hoạt đánh thức
    if (state != POWER_STATE_ACTIVE)
    {
        power_manager_wake();
    }
    else
    {
        portENTER_CRITICAL(&power_mux);
        last_activity_millis = millis();
        ++activity_revision;
        portEXIT_CRITICAL(&power_mux);
    }
}

void power_manager_wake(void)
{
    portENTER_CRITICAL(&power_mux);
    const PowerState prev_state = current_power_state;
    if (prev_state != POWER_STATE_ACTIVE)
    {
        // Bật cờ chặn sự kiện click để ngón tay chạm màn hình không vô tình bấm trúng nút bên dưới
        touch_wake_suppressed = true;
    }

    current_power_state = POWER_STATE_ACTIVE;
    rendering_paused = false;
    last_activity_millis = millis();
    ++activity_revision;
    portEXIT_CRITICAL(&power_mux);

    if (prev_state != POWER_STATE_ACTIVE)
    {
        Serial.printf("[POWER] ⚡ TOUCH TO WAKE: Đánh thức từ trạng thái %s -> ACTIVE (100%%)\n",
            (prev_state == POWER_STATE_DIMMED) ? "DIMMED (20%)" : "SLEEP (0%)");
    }

    // Khôi phục độ sáng 100% ngay lập tức
    lvgl_port_set_brightness(user_active_brightness);
}

void power_manager_sleep(void)
{
    portENTER_CRITICAL(&power_mux);
    current_power_state = POWER_STATE_DISPLAY_SLEEP;
    rendering_paused = true;
    ++activity_revision;
    portEXIT_CRITICAL(&power_mux);
    lvgl_port_set_brightness(POWER_BRIGHTNESS_SLEEP_DEFAULT);
    Serial.println("[POWER] 💤 Buộc chuyển sang chế độ Sleep thủ công");
}

void power_manager_update(void)
{
    uint32_t last_act = 0;
    uint32_t rev_snapshot = 0;
    PowerState state_snapshot = POWER_STATE_ACTIVE;
    uint32_t dim_ms = 0;
    uint32_t sleep_ms = 0;

    portENTER_CRITICAL(&power_mux);
    const uint32_t now = millis();
    last_act = last_activity_millis;
    rev_snapshot = activity_revision;
    state_snapshot = current_power_state;
    dim_ms = timeout_dim_sec * 1000U;
    sleep_ms = timeout_sleep_sec * 1000U;
    portEXIT_CRITICAL(&power_mux);

    // Chống underflow số nguyên không dấu nếu clock chênh lệch hoặc feed vừa cập nhật
    const uint32_t inactive_ms = power_manager_safe_elapsed(now, last_act);

    PowerState target_state = state_snapshot;
    // Trạng thái 2: DIMMED (hoặc ACTIVE) -> Chuyển sang SLEEP sau 120 giây không chạm
    if ((state_snapshot == POWER_STATE_DIMMED || state_snapshot == POWER_STATE_ACTIVE) && 
        inactive_ms >= sleep_ms)
    {
        target_state = POWER_STATE_DISPLAY_SLEEP;
    }
    // Trạng thái 1: ACTIVE -> Chuyển sang DIMMED sau 60 giây không chạm
    else if (state_snapshot == POWER_STATE_ACTIVE && inactive_ms >= dim_ms)
    {
        target_state = POWER_STATE_DIMMED;
    }

    if (target_state != state_snapshot)
    {
        bool applied = false;
        portENTER_CRITICAL(&power_mux);
        // Xác thực revision hoạt động: nếu người dùng vừa chạm màn hình, không chuyển trạng thái ngủ/mờ
        if (power_manager_can_apply_transition(activity_revision, rev_snapshot, current_power_state, state_snapshot))
        {
            current_power_state = target_state;
            if (target_state == POWER_STATE_DISPLAY_SLEEP)
            {
                rendering_paused = true;
            }
            applied = true;
        }
        portEXIT_CRITICAL(&power_mux);

        if (applied)
        {
            if (target_state == POWER_STATE_DIMMED)
            {
                lvgl_port_set_brightness(POWER_BRIGHTNESS_DIM_DEFAULT);
                Serial.printf("[POWER] 🌙 Không hoạt động > %u giây: Chế độ Dimming (Đèn nền hạ xuống 20%%)\n", timeout_dim_sec);
            }
            else if (target_state == POWER_STATE_DISPLAY_SLEEP)
            {
                lvgl_port_set_brightness(POWER_BRIGHTNESS_SLEEP_DEFAULT);
                Serial.printf("[POWER] 💤 Không hoạt động > %u giây: Chế độ Display Sleep (Tắt đèn nền 0%% & Tạm dừng render LVGL)\n", timeout_sleep_sec);
            }
        }
    }
}

PowerState power_manager_get_state(void)
{
    portENTER_CRITICAL(&power_mux);
    const PowerState state = current_power_state;
    portEXIT_CRITICAL(&power_mux);
    return state;
}

bool power_manager_is_rendering_paused(void)
{
    portENTER_CRITICAL(&power_mux);
    const bool paused = rendering_paused;
    portEXIT_CRITICAL(&power_mux);
    return paused;
}

uint32_t power_manager_get_inactivity_seconds(void)
{
    portENTER_CRITICAL(&power_mux);
    const uint32_t now = millis();
    const uint32_t last_act = last_activity_millis;
    portEXIT_CRITICAL(&power_mux);
    const uint32_t inactive_ms = (now >= last_act) ? (now - last_act) : 0;
    return inactive_ms / 1000;
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

uint32_t power_manager_get_activity_revision(void)
{
    portENTER_CRITICAL(&power_mux);
    const uint32_t rev = activity_revision;
    portEXIT_CRITICAL(&power_mux);
    return rev;
}
