/**
 * @file touch_test.cpp
 * @brief Five-point raw FT6336 affine calibration and 40 Hz touch diagnostic.
 */

#include "touch_test.h"
#include "ui_theme.h"
#include "shared_i2c_bus.h"
#include <math.h>

static lv_obj_t *test_container = nullptr;
static lv_obj_t *lbl_info = nullptr;
static lv_obj_t *lbl_instruction = nullptr;
static lv_obj_t *crosshair = nullptr;
static lv_timer_t *touch_timer = nullptr;

struct TargetPoint
{
    const char *name;
    lv_coord_t x;
    lv_coord_t y;
    lv_obj_t *circle;
};

static TargetPoint targets[TOUCH_CALIBRATION_POINT_COUNT] = {
    {"Top Left", 24, 72, nullptr}, {"Top Right", 216, 72, nullptr},
    {"Bottom Left", 24, 236, nullptr}, {"Bottom Right", 216, 236, nullptr},
    {"Center", 120, 154, nullptr}
};
static TouchCalibrationPoint calibration_points[TOUCH_CALIBRATION_POINT_COUNT];
static uint16_t sample_x[24];
static uint16_t sample_y[24];
static uint8_t sample_count = 0;
static uint8_t target_index = 0;
static bool calibrating = false;
static bool wait_for_release = true;
static float last_rms = 0.0f;
static float last_max = 0.0f;

static void sort_u16(uint16_t *values, size_t count)
{
    for (size_t i = 1; i < count; ++i)
    {
        uint16_t value = values[i];
        size_t j = i;
        while (j > 0 && values[j - 1] > value)
        {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

static void set_target_active(uint8_t active)
{
    for (uint8_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i)
    {
        if (!targets[i].circle) continue;
        bool selected = i == active;
        lv_obj_set_style_bg_color(targets[i].circle,
                                  lv_color_hex(selected ? COLOR_ACCENT_PURPLE : 0x1F2A3D), 0);
        lv_obj_set_style_border_color(targets[i].circle,
                                      lv_color_hex(selected ? COLOR_TEXT_WHITE : COLOR_CARD_BORDER), 0);
    }
}

static void begin_calibration(void)
{
    shared_i2c_touch_reset_calibration();
    sample_count = 0;
    target_index = 0;
    calibrating = true;
    wait_for_release = true;
    last_rms = 0.0f;
    last_max = 0.0f;
    set_target_active(0);
    lv_obj_set_style_text_color(lbl_instruction, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_label_set_text_fmt(lbl_instruction, "Nhấn giữ: %s (>=15 mẫu)", targets[0].name);
}

static void reset_button_cb(lv_event_t *event)
{
    (void)event;
    begin_calibration();
}

static void finish_target(void)
{
    sort_u16(sample_x, sample_count);
    sort_u16(sample_y, sample_count);
    const uint8_t trim = 2;
    uint32_t sum_x = 0, sum_y = 0;
    for (uint8_t i = trim; i < sample_count - trim; ++i)
    {
        sum_x += sample_x[i];
        sum_y += sample_y[i];
    }
    const uint8_t used = sample_count - trim * 2;
    calibration_points[target_index].raw_x = (float)sum_x / used;
    calibration_points[target_index].raw_y = (float)sum_y / used;
    lv_area_t area;
    lv_obj_get_coords(test_container, &area);
    calibration_points[target_index].screen_x = area.x1 + targets[target_index].x;
    calibration_points[target_index].screen_y = area.y1 + targets[target_index].y;

    ++target_index;
    sample_count = 0;
    wait_for_release = true;
    if (target_index < TOUCH_CALIBRATION_POINT_COUNT)
    {
        set_target_active(target_index);
        lv_label_set_text_fmt(lbl_instruction, "Nhấn giữ: %s (>=15 mẫu)", targets[target_index].name);
        return;
    }

    bool ok = shared_i2c_touch_calibrate(calibration_points, TOUCH_CALIBRATION_POINT_COUNT,
                                         &last_rms, &last_max);
    calibrating = false;
    set_target_active(255);
    lv_obj_set_style_text_color(lbl_instruction,
                                lv_color_hex(ok ? COLOR_ACCENT_GREEN : COLOR_ACCENT_RED), 0);
    lv_label_set_text_fmt(lbl_instruction, ok ? "VALID  RMS %.1f  Max %.1f px"
                                               : "FAILED  RMS %.1f  Max %.1f px - Reset",
                          last_rms, last_max);
}

static void touch_test_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_touch_test_update();
}

void ui_touch_test_open(lv_obj_t *parent)
{
    test_container = parent;
    lv_obj_clean(parent);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A0D14), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    targets[0].x = 24; targets[0].y = 24;
    targets[1].x = SCREEN_WIDTH - 24; targets[1].y = 24;
    targets[2].x = 24; targets[2].y = APP_CONTENT_HEIGHT - 24;
    targets[3].x = SCREEN_WIDTH - 24; targets[3].y = APP_CONTENT_HEIGHT - 24;
    targets[4].x = SCREEN_WIDTH / 2; targets[4].y = APP_CONTENT_HEIGHT / 2;

    lv_obj_t *reset_btn = lv_btn_create(parent);
    lv_obj_set_size(reset_btn, 64, 32);
    lv_obj_align(reset_btn, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_add_event_cb(reset_btn, reset_button_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *reset_label = lv_label_create(reset_btn);
    lv_label_set_text(reset_label, "Reset");
    lv_obj_set_style_text_font(reset_label, UI_FONT_BUTTON, 0);
    lv_obj_center(reset_label);

    lbl_info = lv_label_create(parent);
    lv_obj_set_width(lbl_info, SCREEN_WIDTH - 8);
    lv_label_set_text(lbl_info, "Raw: --,--  Screen: --,--\nError: -- px  Calibration: INVALID  RMS: -- px");
    lv_obj_set_style_text_color(lbl_info, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_info, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_info, LV_ALIGN_TOP_LEFT, 4, 40);

    for (uint8_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i)
    {
        targets[i].circle = lv_obj_create(parent);
        lv_obj_set_size(targets[i].circle, 36, 36);
        lv_obj_set_pos(targets[i].circle, targets[i].x - 18, targets[i].y - 18);
        lv_obj_set_style_radius(targets[i].circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(targets[i].circle, lv_color_hex(0x1F2A3D), 0);
        lv_obj_set_style_border_width(targets[i].circle, 2, 0);
        lv_obj_set_style_pad_all(targets[i].circle, 0, 0);
        lv_obj_clear_flag(targets[i].circle, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *mark = lv_label_create(targets[i].circle);
        lv_label_set_text(mark, i == 4 ? "+" : "•");
        lv_obj_set_style_text_color(mark, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_set_style_text_font(mark, UI_FONT_BODY, 0);
        lv_obj_center(mark);
    }

    crosshair = lv_obj_create(parent);
    lv_obj_set_size(crosshair, 20, 20);
    lv_obj_set_pos(crosshair, -100, -100);
    lv_obj_set_style_radius(crosshair, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(crosshair, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(crosshair, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_border_width(crosshair, 2, 0);
    lv_obj_clear_flag(crosshair, LV_OBJ_FLAG_SCROLLABLE);

    lbl_instruction = lv_label_create(parent);
    lv_obj_set_width(lbl_instruction, SCREEN_WIDTH - 8);
    lv_obj_set_style_text_align(lbl_instruction, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_instruction, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_instruction, LV_ALIGN_TOP_MID, 0, 84);

    TouchCalibration cal = shared_i2c_touch_get_calibration();
    if (cal.valid)
    {
        calibrating = false;
        last_rms = cal.rms_error;
        last_max = cal.max_error;
        set_target_active(255);
        lv_obj_set_style_text_color(lbl_instruction, lv_color_hex(COLOR_ACCENT_GREEN), 0);
        lv_label_set_text_fmt(lbl_instruction, "VALID  RMS %.1f  Max %.1f px", last_rms, last_max);
    }
    else begin_calibration();

    if (!touch_timer) touch_timer = lv_timer_create(touch_test_timer_cb, 25, nullptr);
}

void ui_touch_test_update(void)
{
    if (!test_container) return;
    uint16_t raw_x = 0, raw_y = 0, screen_x = 0, screen_y = 0;
    bool touched = shared_i2c_touch_read_debug(&raw_x, &raw_y, &screen_x, &screen_y);
    TouchCalibration cal = shared_i2c_touch_get_calibration();
    lv_area_t area;
    lv_obj_get_coords(test_container, &area);
    int local_x = (int)screen_x - area.x1;
    int local_y = (int)screen_y - area.y1;
    float error = 0.0f;

    if (touched)
    {
        lv_obj_set_pos(crosshair, local_x - 10, local_y - 10);
        if (calibrating && !wait_for_release)
        {
            if (sample_count < 24)
            {
                sample_x[sample_count] = raw_x;
                sample_y[sample_count] = raw_y;
                ++sample_count;
            }
            if (sample_count >= 15)
            {
                uint16_t min_x = sample_x[0], max_x = sample_x[0];
                uint16_t min_y = sample_y[0], max_y = sample_y[0];
                for (uint8_t i = 1; i < sample_count; ++i)
                {
                    if (sample_x[i] < min_x) min_x = sample_x[i];
                    if (sample_x[i] > max_x) max_x = sample_x[i];
                    if (sample_y[i] < min_y) min_y = sample_y[i];
                    if (sample_y[i] > max_y) max_y = sample_y[i];
                }
                if ((max_x - min_x) <= 18 && (max_y - min_y) <= 18) finish_target();
                else if (sample_count == 24)
                {
                    sample_count = 0;
                    lv_label_set_text(lbl_instruction, "Chạm chưa ổn định - giữ yên và thử lại");
                }
            }
        }
        else if (!calibrating)
        {
            error = 10000.0f;
            for (uint8_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i)
            {
                float dx = local_x - targets[i].x;
                float dy = local_y - targets[i].y;
                float distance = sqrtf(dx * dx + dy * dy);
                if (distance < error) error = distance;
            }
        }
    }
    else wait_for_release = false;

    lv_label_set_text_fmt(lbl_info,
                          "Raw: %u,%u  Screen: %u,%u\nError: %.1f px  Calibration: %s  RMS: %.1f px",
                          raw_x, raw_y, screen_x, screen_y, error,
                          cal.valid ? "VALID" : "INVALID", cal.valid ? cal.rms_error : last_rms);
}

void ui_touch_test_close(void)
{
    if (touch_timer) { lv_timer_del(touch_timer); touch_timer = nullptr; }
    test_container = nullptr;
    lbl_info = nullptr;
    lbl_instruction = nullptr;
    crosshair = nullptr;
    for (uint8_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i) targets[i].circle = nullptr;
}
