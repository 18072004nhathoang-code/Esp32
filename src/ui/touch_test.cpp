/**
 * @file touch_test.cpp
 * @brief Triển khai màn hình kiểm tra cảm ứng (Touch Test & Calibration Pattern) cho 320x240
 */

#include "touch_test.h"
#include "ui_theme.h"
#include "shared_i2c_bus.h"
#include <stdio.h>
#include <math.h>

static lv_obj_t *test_container = nullptr;
static lv_obj_t *lbl_info = nullptr;
static lv_obj_t *crosshair = nullptr;
static lv_timer_t *touch_timer = nullptr;

struct TargetPoint {
    const char *name;
    lv_coord_t x;
    lv_coord_t y;
    lv_obj_t *circle;
    lv_obj_t *lbl;
    bool hit;
    int min_err;
};

static TargetPoint targets[5] = {
    {"TL", 24, 24, nullptr, nullptr, false, 999},
    {"TR", 296, 24, nullptr, nullptr, false, 999},
    {"BL", 24, 170, nullptr, nullptr, false, 999},
    {"BR", 296, 170, nullptr, nullptr, false, 999},
    {"Center", 160, 97, nullptr, nullptr, false, 999}
};

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

    // Header info
    lbl_info = lv_label_create(parent);
    lv_label_set_text(lbl_info, "Chạm vào màn hình để kiểm tra tọa độ và 5 điểm chuẩn");
    lv_obj_set_style_text_color(lbl_info, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_text_font(lbl_info, UI_FONT_12, 0);
    lv_obj_align(lbl_info, LV_ALIGN_TOP_MID, 0, 4);

    // Tính toán lại vị trí 5 điểm mục tiêu dựa trên kích thước thật
    targets[0].x = 26;
    targets[0].y = 26;

    targets[1].x = SCREEN_WIDTH - 26;
    targets[1].y = 26;

    targets[2].x = 26;
    targets[2].y = APP_CONTENT_HEIGHT - 26;

    targets[3].x = SCREEN_WIDTH - 26;
    targets[3].y = APP_CONTENT_HEIGHT - 26;

    targets[4].x = SCREEN_WIDTH / 2;
    targets[4].y = APP_CONTENT_HEIGHT / 2;

    for (int i = 0; i < 5; i++)
    {
        targets[i].hit = false;
        targets[i].min_err = 999;

        targets[i].circle = lv_obj_create(parent);
        lv_obj_set_size(targets[i].circle, 36, 36);
        lv_obj_set_pos(targets[i].circle, targets[i].x - 18, targets[i].y - 18);
        lv_obj_set_style_radius(targets[i].circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(targets[i].circle, lv_color_hex(0x1F2A3D), 0);
        lv_obj_set_style_border_color(targets[i].circle, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
        lv_obj_set_style_border_width(targets[i].circle, 2, 0);
        lv_obj_clear_flag(targets[i].circle, LV_OBJ_FLAG_SCROLLABLE);

        targets[i].lbl = lv_label_create(targets[i].circle);
        lv_label_set_text(targets[i].lbl, targets[i].name);
        lv_obj_set_style_text_color(targets[i].lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_set_style_text_font(targets[i].lbl, UI_FONT_TINY, 0);
        lv_obj_center(targets[i].lbl);
    }

    // Crosshair (vòng tròn tâm đỏ di động)
    crosshair = lv_obj_create(parent);
    lv_obj_set_size(crosshair, 20, 20);
    lv_obj_set_pos(crosshair, -100, -100);
    lv_obj_set_style_radius(crosshair, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(crosshair, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(crosshair, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_border_width(crosshair, 2, 0);
    lv_obj_clear_flag(crosshair, LV_OBJ_FLAG_SCROLLABLE);

    // Dedicated timer 25ms (40Hz) để refresh mượt mà
    if (!touch_timer)
    {
        touch_timer = lv_timer_create(touch_test_timer_cb, 25, NULL);
    }
}

void ui_touch_test_update(void)
{
    if (!test_container) return;

    uint16_t rx = 0, ry = 0, mx = 0, my = 0;
    bool touched = shared_i2c_touch_read_debug(&rx, &ry, &mx, &my);

    if (touched)
    {
        lv_area_t a;
        lv_obj_get_coords(test_container, &a);
        int16_t local_x = (int16_t)mx - a.x1;
        int16_t local_y = (int16_t)my - a.y1;

        if (crosshair)
        {
            lv_obj_set_pos(crosshair, local_x - 10, local_y - 10);
        }

        // Kiểm tra khoảng cách tới 5 điểm chuẩn (dùng local coordinates)
        for (int i = 0; i < 5; i++)
        {
            int dx = (int)local_x - (int)targets[i].x;
            int dy = (int)local_y - (int)targets[i].y;
            int dist = (int)sqrt(dx * dx + dy * dy);

            if (dist < targets[i].min_err)
            {
                targets[i].min_err = dist;
            }

            if (dist <= 18)
            {
                targets[i].hit = true;
                if (targets[i].circle)
                {
                    lv_obj_set_style_bg_color(targets[i].circle, lv_color_hex(COLOR_ACCENT_GREEN), 0);
                    lv_obj_set_style_border_color(targets[i].circle, lv_color_hex(0xFFFFFF), 0);
                }
                if (targets[i].lbl)
                {
                    lv_label_set_text_fmt(targets[i].lbl, "E:%d", targets[i].min_err);
                }
            }
        }

        if (lbl_info)
        {
            lv_label_set_text_fmt(lbl_info, "Raw: (%u, %u) | Mapped: (%u, %u)", rx, ry, mx, my);
            lv_obj_set_style_text_color(lbl_info, lv_color_hex(COLOR_ACCENT_GREEN), 0);
        }
    }
}

void ui_touch_test_close(void)
{
    if (touch_timer)
    {
        lv_timer_del(touch_timer);
        touch_timer = nullptr;
    }
    test_container = nullptr;
    lbl_info = nullptr;
    crosshair = nullptr;
    for (int i = 0; i < 5; i++)
    {
        targets[i].circle = nullptr;
        targets[i].lbl = nullptr;
    }
}
