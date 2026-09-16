/**
 * @file touch_test.cpp
 * @brief Five-point FT6336 affine calibration and independent validation.
 */

#include "touch_test.h"
#include "ui_theme.h"
#include "shared_i2c_bus.h"
#include <math.h>

namespace
{
constexpr uint8_t kSamplesRequired = 15;
constexpr uint8_t kSamplesCapacity = 24;
constexpr uint8_t kTrimCount = 2;
constexpr float kValidationRmsLimit = 8.0f;
constexpr float kValidationMaxLimit = 12.0f;

enum class CalibrationPhase : uint8_t
{
    Diagnostic,
    Fit,
    Validate,
    WaitRetryRelease,
    WaitSuccessRelease,
};

struct TargetPoint
{
    const char *name;
    lv_coord_t x;
    lv_coord_t y;
    lv_obj_t *circle;
};

static lv_obj_t *test_container = nullptr;
static lv_obj_t *lbl_info = nullptr;
static lv_obj_t *lbl_instruction = nullptr;
static lv_obj_t *crosshair = nullptr;
static lv_timer_t *touch_timer = nullptr;

static TargetPoint targets[TOUCH_CALIBRATION_POINT_COUNT] = {
    {"Top Left", 24, 24, nullptr},
    {"Top Right", SCREEN_WIDTH - 24, 24, nullptr},
    {"Bottom Left", 24, APP_CONTENT_HEIGHT - 24, nullptr},
    {"Bottom Right", SCREEN_WIDTH - 24, APP_CONTENT_HEIGHT - 24, nullptr},
    {"Center", SCREEN_WIDTH / 2, APP_CONTENT_HEIGHT / 2, nullptr},
};

static const TargetPoint validation_targets[TOUCH_CALIBRATION_POINT_COUNT] = {
    {"Top", SCREEN_WIDTH / 2, 50, nullptr},
    {"Left", 45, APP_CONTENT_HEIGHT / 2, nullptr},
    {"Right", SCREEN_WIDTH - 45, APP_CONTENT_HEIGHT / 2, nullptr},
    {"Bottom", SCREEN_WIDTH / 2, APP_CONTENT_HEIGHT - 50, nullptr},
    {"Inner", 82, 104, nullptr},
};

static TouchCalibrationPoint fit_points[TOUCH_CALIBRATION_POINT_COUNT];
static TouchCalibrationPoint validation_points[TOUCH_CALIBRATION_POINT_COUNT];
static TouchCalibration candidate = {};
static uint16_t sample_x[kSamplesCapacity];
static uint16_t sample_y[kSamplesCapacity];
static uint8_t sample_count = 0;
static uint8_t target_index = 0;
static uint32_t last_sample_sequence = 0;
static bool wait_for_release = true;
static bool forced_calibration = false;
static float fit_rms = 0.0f;
static float fit_max = 0.0f;
static float validation_rms = 0.0f;
static float validation_max = 0.0f;
static volatile CalibrationPhase phase = CalibrationPhase::Diagnostic;

static void sort_u16(uint16_t *values, size_t count)
{
    for (size_t i = 1; i < count; ++i)
    {
        const uint16_t value = values[i];
        size_t j = i;
        while (j > 0 && values[j - 1] > value)
        {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

static bool phase_is_blocking()
{
    return phase != CalibrationPhase::Diagnostic;
}

static const char *phase_name()
{
    switch (phase)
    {
        case CalibrationPhase::Fit: return "FIT";
        case CalibrationPhase::Validate: return "VALIDATE";
        case CalibrationPhase::WaitRetryRelease: return "RETRY";
        case CalibrationPhase::WaitSuccessRelease: return "SAVED";
        default: return "TEST";
    }
}

static const TargetPoint &current_target()
{
    return phase == CalibrationPhase::Validate ? validation_targets[target_index]
                                                : targets[target_index];
}

static void place_target_markers()
{
    for (uint8_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i)
    {
        if (!targets[i].circle) continue;
        const TargetPoint &point = phase == CalibrationPhase::Validate ? validation_targets[i]
                                                                       : targets[i];
        lv_obj_set_pos(targets[i].circle, point.x - 18, point.y - 18);
        const bool selected = phase_is_blocking() && i == target_index &&
                              phase != CalibrationPhase::WaitRetryRelease &&
                              phase != CalibrationPhase::WaitSuccessRelease;
        lv_obj_set_style_bg_color(targets[i].circle,
                                  lv_color_hex(selected ? COLOR_ACCENT_PURPLE : 0x1F2A3D), 0);
        lv_obj_set_style_border_color(targets[i].circle,
                                      lv_color_hex(selected ? COLOR_TEXT_WHITE : COLOR_CARD_BORDER), 0);
    }
}

static void set_instruction_for_target()
{
    const TargetPoint &point = current_target();
    lv_obj_set_style_text_color(lbl_instruction, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_label_set_text_fmt(lbl_instruction,
                          phase == CalibrationPhase::Validate
                              ? "Xác minh mới: %s (giữ đủ %u mẫu)"
                              : "Nhấn giữ: %s (giữ đủ %u mẫu)",
                          point.name, kSamplesRequired);
}

static void begin_calibration()
{
    sample_count = 0;
    target_index = 0;
    last_sample_sequence = 0;
    wait_for_release = true;
    fit_rms = fit_max = validation_rms = validation_max = 0.0f;
    candidate = {};
    phase = CalibrationPhase::Fit;
    shared_i2c_touch_set_ui_suppressed(true);
    place_target_markers();
    set_instruction_for_target();
}

static void reset_button_cb(lv_event_t *event)
{
    (void)event;
    begin_calibration();
}

static bool stable_sample_average(float *raw_x, float *raw_y)
{
    if (sample_count < kSamplesRequired) return false;

    uint16_t min_x = sample_x[0], max_x = sample_x[0];
    uint16_t min_y = sample_y[0], max_y = sample_y[0];
    for (uint8_t i = 1; i < sample_count; ++i)
    {
        if (sample_x[i] < min_x) min_x = sample_x[i];
        if (sample_x[i] > max_x) max_x = sample_x[i];
        if (sample_y[i] < min_y) min_y = sample_y[i];
        if (sample_y[i] > max_y) max_y = sample_y[i];
    }
    if ((max_x - min_x) > 18 || (max_y - min_y) > 18) return false;

    sort_u16(sample_x, sample_count);
    sort_u16(sample_y, sample_count);
    uint32_t sum_x = 0;
    uint32_t sum_y = 0;
    for (uint8_t i = kTrimCount; i < sample_count - kTrimCount; ++i)
    {
        sum_x += sample_x[i];
        sum_y += sample_y[i];
    }
    const uint8_t used = sample_count - 2 * kTrimCount;
    *raw_x = (float)sum_x / used;
    *raw_y = (float)sum_y / used;
    return true;
}

static void begin_validation()
{
    phase = CalibrationPhase::Validate;
    target_index = 0;
    sample_count = 0;
    wait_for_release = true;
    place_target_markers();
    set_instruction_for_target();
}

static void fail_attempt(const char *message)
{
    phase = CalibrationPhase::WaitRetryRelease;
    sample_count = 0;
    place_target_markers();
    lv_obj_set_style_text_color(lbl_instruction, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_label_set_text_fmt(lbl_instruction, "%s - nhấc tay để thử lại", message);
}

static void finish_validation()
{
    float sum_squared = 0.0f;
    validation_max = 0.0f;
    for (uint8_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i)
    {
        float mapped_x = 0.0f;
        float mapped_y = 0.0f;
        shared_i2c_touch_map_raw(&candidate,
                                 (uint16_t)lroundf(validation_points[i].raw_x),
                                 (uint16_t)lroundf(validation_points[i].raw_y),
                                 &mapped_x, &mapped_y);
        const float dx = (float)mapped_x - validation_points[i].screen_x;
        const float dy = (float)mapped_y - validation_points[i].screen_y;
        const float error = sqrtf(dx * dx + dy * dy);
        sum_squared += error * error;
        if (error > validation_max) validation_max = error;
    }
    validation_rms = sqrtf(sum_squared / TOUCH_CALIBRATION_POINT_COUNT);

    if (!shared_i2c_touch_commit_calibration(&candidate, validation_rms, validation_max))
    {
        fail_attempt("Validation FAILED");
        return;
    }

    phase = CalibrationPhase::WaitSuccessRelease;
    sample_count = 0;
    place_target_markers();
    lv_obj_set_style_text_color(lbl_instruction, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_label_set_text_fmt(lbl_instruction, "VALID  RMS %.1f  Max %.1f px - nhấc tay",
                          validation_rms, validation_max);
}

static void finish_target()
{
    float raw_x = 0.0f;
    float raw_y = 0.0f;
    if (!stable_sample_average(&raw_x, &raw_y))
    {
        if (sample_count == kSamplesCapacity)
        {
            sample_count = 0;
            lv_label_set_text(lbl_instruction, "Chạm chưa ổn định - giữ yên và thử lại");
        }
        return;
    }

    lv_area_t area;
    lv_obj_get_coords(test_container, &area);
    const TargetPoint &target = current_target();
    TouchCalibrationPoint *points = phase == CalibrationPhase::Fit ? fit_points : validation_points;
    points[target_index] = {
        raw_x,
        raw_y,
        (float)(area.x1 + target.x),
        (float)(area.y1 + target.y),
    };

    ++target_index;
    sample_count = 0;
    wait_for_release = true;
    if (target_index < TOUCH_CALIBRATION_POINT_COUNT)
    {
        place_target_markers();
        set_instruction_for_target();
        return;
    }

    if (phase == CalibrationPhase::Fit)
    {
        if (!shared_i2c_touch_solve(fit_points, TOUCH_CALIBRATION_POINT_COUNT,
                                    &candidate, &fit_rms, &fit_max))
        {
            fail_attempt("Fit FAILED");
            return;
        }
        begin_validation();
    }
    else
    {
        finish_validation();
    }
}

static void touch_test_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_touch_test_update();
}
} // namespace

void ui_touch_test_request_forced_calibration(void)
{
    forced_calibration = true;
}

bool ui_touch_test_should_auto_open(void)
{
    return forced_calibration || !shared_i2c_touch_get_calibration().valid;
}

bool ui_touch_test_is_calibration_blocking(void)
{
    return phase_is_blocking();
}

void ui_touch_test_open(lv_obj_t *parent)
{
    test_container = parent;
    lv_obj_clean(parent);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A0D14), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *reset_btn = lv_btn_create(parent);
    lv_obj_set_size(reset_btn, 132, 32);
    lv_obj_align(reset_btn, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_bg_color(reset_btn, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_add_event_cb(reset_btn, reset_button_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *reset_label = lv_label_create(reset_btn);
    lv_label_set_text(reset_label, "Reset Calibration");
    lv_obj_set_style_text_font(reset_label, UI_FONT_BUTTON, 0);
    lv_obj_center(reset_label);

    lbl_info = lv_label_create(parent);
    lv_obj_set_width(lbl_info, SCREEN_WIDTH - 8);
    lv_label_set_text(lbl_info, "Raw: --,--  Mapped: --,--\nError: -- px  Calibration: INVALID  Phase: TEST");
    lv_obj_set_style_text_color(lbl_info, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_info, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_info, LV_ALIGN_TOP_LEFT, 4, 38);

    for (uint8_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i)
    {
        targets[i].circle = lv_obj_create(parent);
        lv_obj_set_size(targets[i].circle, 36, 36);
        lv_obj_set_style_radius(targets[i].circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(targets[i].circle, lv_color_hex(0x1F2A3D), 0);
        lv_obj_set_style_border_width(targets[i].circle, 2, 0);
        lv_obj_set_style_pad_all(targets[i].circle, 0, 0);
        lv_obj_clear_flag(targets[i].circle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
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
    lv_obj_clear_flag(crosshair, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lbl_instruction = lv_label_create(parent);
    lv_obj_set_width(lbl_instruction, SCREEN_WIDTH - 8);
    lv_obj_set_style_text_align(lbl_instruction, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl_instruction, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_instruction, LV_ALIGN_TOP_MID, 0, 82);

    const TouchCalibration cal = shared_i2c_touch_get_calibration();
    if (forced_calibration || !cal.valid)
    {
        forced_calibration = false;
        begin_calibration();
    }
    else
    {
        phase = CalibrationPhase::Diagnostic;
        shared_i2c_touch_set_ui_suppressed(false);
        place_target_markers();
        lv_obj_set_style_text_color(lbl_instruction, lv_color_hex(COLOR_ACCENT_GREEN), 0);
        lv_label_set_text_fmt(lbl_instruction, "VALID  RMS %.1f  Max %.1f px", cal.rms_error, cal.max_error);
    }

    if (!touch_timer) touch_timer = lv_timer_create(touch_test_timer_cb, 25, nullptr);
}

void ui_touch_test_update(void)
{
    if (!test_container) return;

    uint16_t raw_x = 0, raw_y = 0, screen_x = 0, screen_y = 0;
    uint32_t sequence = 0;
    const bool touched = shared_i2c_touch_read_debug(&raw_x, &raw_y, &screen_x, &screen_y, &sequence);
    const TouchCalibration cal = shared_i2c_touch_get_calibration();
    lv_area_t area;
    lv_obj_get_coords(test_container, &area);
    const int local_x = (int)screen_x - area.x1;
    const int local_y = (int)screen_y - area.y1;
    float error = 0.0f;

    if (!touched)
    {
        lv_obj_set_pos(crosshair, -100, -100);
        if (!wait_for_release && sample_count > 0) sample_count = 0;
        wait_for_release = false;

        if (phase == CalibrationPhase::WaitRetryRelease)
        {
            begin_calibration();
        }
        else if (phase == CalibrationPhase::WaitSuccessRelease)
        {
            phase = CalibrationPhase::Diagnostic;
            shared_i2c_touch_set_ui_suppressed(false);
            place_target_markers();
        }
    }
    else
    {
        lv_obj_set_pos(crosshair, local_x - 10, local_y - 10);
        if ((phase == CalibrationPhase::Fit || phase == CalibrationPhase::Validate) &&
            !wait_for_release && sequence != last_sample_sequence)
        {
            last_sample_sequence = sequence;
            if (sample_count < kSamplesCapacity)
            {
                sample_x[sample_count] = raw_x;
                sample_y[sample_count] = raw_y;
                ++sample_count;
            }
            finish_target();
        }

        if (phase == CalibrationPhase::Validate)
        {
            float candidate_x = 0.0f;
            float candidate_y = 0.0f;
            shared_i2c_touch_map_raw(&candidate, raw_x, raw_y, &candidate_x, &candidate_y);
            const TargetPoint &target = current_target();
            const float dx = (float)candidate_x - (area.x1 + target.x);
            const float dy = (float)candidate_y - (area.y1 + target.y);
            error = sqrtf(dx * dx + dy * dy);
            lv_obj_set_pos(crosshair, (int)lroundf(candidate_x) - area.x1 - 10,
                           (int)lroundf(candidate_y) - area.y1 - 10);
        }
        else if (phase == CalibrationPhase::Diagnostic && cal.valid)
        {
            error = 10000.0f;
            for (uint8_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i)
            {
                const float dx = local_x - targets[i].x;
                const float dy = local_y - targets[i].y;
                const float distance = sqrtf(dx * dx + dy * dy);
                if (distance < error) error = distance;
            }
        }
    }

    lv_label_set_text_fmt(lbl_info,
                          "Raw: %u,%u  Mapped: %u,%u\nError: %.1f px  Calibration: %s  Phase: %s",
                          raw_x, raw_y, screen_x, screen_y, error,
                          cal.valid ? "VALID" : "INVALID", phase_name());
}

void ui_touch_test_close(void)
{
    if (phase_is_blocking()) return;
    if (touch_timer)
    {
        lv_timer_del(touch_timer);
        touch_timer = nullptr;
    }
    shared_i2c_touch_set_ui_suppressed(false);
    test_container = nullptr;
    lbl_info = nullptr;
    lbl_instruction = nullptr;
    crosshair = nullptr;
    for (uint8_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i) targets[i].circle = nullptr;
}
