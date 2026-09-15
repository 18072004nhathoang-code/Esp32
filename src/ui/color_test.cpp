/**
 * @file color_test.cpp
 * @brief Triển khai màn hình kiểm tra màu phần cứng (Hardware Color Self-Test)
 */

#include "color_test.h"
#include "ui_theme.h"
#include <stdio.h>

struct ColorBarInfo {
    const char *name;
    uint32_t hex;
    uint32_t text_color;
};

static const ColorBarInfo COLOR_BARS[8] = {
    {"RED (#FF0000)",     0xFF0000, 0xFFFFFF},
    {"GREEN (#00FF00)",   0x00FF00, 0x000000},
    {"BLUE (#0000FF)",    0x0000FF, 0xFFFFFF},
    {"WHITE (#FFFFFF)",   0xFFFFFF, 0x000000},
    {"BLACK (#000000)",   0x000000, 0xFFFFFF},
    {"YELLOW (#FFFF00)",  0xFFFF00, 0x000000},
    {"CYAN (#00FFFF)",    0x00FFFF, 0x000000},
    {"MAGENTA (#FF00FF)", 0xFF00FF, 0xFFFFFF}
};

void ui_color_test_open(lv_obj_t *parent)
{
    lv_obj_clean(parent);
    lv_obj_set_style_pad_all(parent, 2, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x000000), 0);

    // Tính toán chiều cao mỗi thanh màu phù hợp vùng hiển thị
    lv_coord_t total_h = APP_CONTENT_HEIGHT - 32;
    lv_coord_t bar_h = total_h / 8;
    if (bar_h < 26) bar_h = 26;

    for (int i = 0; i < 8; i++)
    {
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_set_size(bar, SCREEN_WIDTH - 6, bar_h);
        lv_obj_set_pos(bar, 3, i * (bar_h + 1));
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_BARS[i].hex), 0);
        lv_obj_set_style_border_width(bar, 1, 0);
        lv_obj_set_style_border_color(bar, lv_color_hex(0x333333), 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(bar);
        lv_label_set_text(lbl, COLOR_BARS[i].name);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_BARS[i].text_color), 0);
        lv_obj_set_style_text_font(lbl, UI_FONT_10, 0);
        lv_obj_center(lbl);
    }

    // Thanh hướng dẫn chẩn đoán dưới đáy
    lv_obj_t *diag_box = lv_obj_create(parent);
    lv_obj_set_size(diag_box, SCREEN_WIDTH - 6, 26);
    lv_obj_align(diag_box, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_obj_set_style_bg_color(diag_box, lv_color_hex(0x111620), 0);
    lv_obj_set_style_border_color(diag_box, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_border_width(diag_box, 1, 0);
    lv_obj_set_style_radius(diag_box, 4, 0);
    lv_obj_set_style_pad_all(diag_box, 0, 0);
    lv_obj_clear_flag(diag_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *diag_lbl = lv_label_create(diag_box);
    lv_label_set_text(diag_lbl, "Kiem tra: Do=Red, Xanh=Green/Blue");
    lv_obj_set_style_text_color(diag_lbl, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_text_font(diag_lbl, UI_FONT_10, 0);
    lv_obj_center(diag_lbl);
}
