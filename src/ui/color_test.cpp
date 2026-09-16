/** @file color_test.cpp @brief Runtime display A/B diagnostic. */

#include "color_test.h"
#include "ui_theme.h"
#include "../display/lvgl_port.h"

struct ColorBarInfo { const char *name; uint32_t hex; uint32_t text; };
static const ColorBarInfo bars[] = {
    {"RED", 0xFF0000, 0xFFFFFF}, {"GREEN", 0x00FF00, 0x000000},
    {"BLUE", 0x0000FF, 0xFFFFFF}, {"WHITE", 0xFFFFFF, 0x000000},
    {"BLACK", 0x000000, 0xFFFFFF}, {"CYAN", 0x00FFFF, 0x000000},
    {"MAGENTA", 0xFF00FF, 0xFFFFFF}, {"YELLOW", 0xFFFF00, 0x000000}
};
static lv_obj_t *state_label = nullptr;

static void refresh_state(void)
{
    DisplayDiagnosticState state = lvgl_port_get_display_diagnostic();
    lv_label_set_text_fmt(state_label, "RGB565 swap %s | %s | invert %s",
                          state.swap_bytes ? "ON" : "OFF",
                          state.bgr_order ? "BGR" : "RGB",
                          state.inverted ? "ON" : "OFF");
}

static void toggle_cb(lv_event_t *event)
{
    uintptr_t field = (uintptr_t)lv_event_get_user_data(event);
    DisplayDiagnosticState state = lvgl_port_get_display_diagnostic();
    if (field == 0) state.swap_bytes = !state.swap_bytes;
    else if (field == 1) state.bgr_order = !state.bgr_order;
    else state.inverted = !state.inverted;
    lvgl_port_set_display_diagnostic(state);
    refresh_state();
}

static void apply_cb(lv_event_t *event)
{
    (void)event;
    bool ok = lvgl_port_apply_display_diagnostic();
    refresh_state();
    if (!ok) lv_label_set_text(state_label, "Apply failed - cấu hình chưa được lưu");
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_coord_t x,
                             lv_event_cb_t callback, uintptr_t user_data)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, 54, 34);
    lv_obj_set_pos(button, x, 224);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1F2A3D), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)user_data);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UI_FONT_BUTTON, 0);
    lv_obj_center(label);
    return button;
}

void ui_color_test_open(lv_obj_t *parent)
{
    lv_obj_clean(parent);
    lv_obj_set_style_pad_all(parent, 2, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x080A0E), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t gap = 2;
    const lv_coord_t width = (SCREEN_WIDTH - 6) / 2;
    for (int i = 0; i < 8; ++i)
    {
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_set_size(bar, width, 26);
        lv_obj_set_pos(bar, 2 + (i % 2) * (width + gap), 2 + (i / 2) * 28);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(bars[i].hex), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *label = lv_label_create(bar);
        lv_label_set_text(label, bars[i].name);
        lv_obj_set_style_text_color(label, lv_color_hex(bars[i].text), 0);
        lv_obj_set_style_text_font(label, UI_FONT_SMALL, 0);
        lv_obj_center(label);
    }

    lv_obj_t *sample = lv_label_create(parent);
    lv_obj_set_width(sample, SCREEN_WIDTH - 8);
    lv_label_set_text(sample, "ABC abc 123\nQuét  Kết nối  Mật khẩu\nTiếng Việt  Hoàng");
    lv_obj_set_style_text_align(sample, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(sample, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(sample, UI_FONT_BODY, 0);
    lv_obj_set_pos(sample, 4, 118);

    state_label = lv_label_create(parent);
    lv_obj_set_width(state_label, SCREEN_WIDTH - 8);
    lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(state_label, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_text_font(state_label, UI_FONT_SMALL, 0);
    lv_obj_set_pos(state_label, 4, 191);
    refresh_state();

    make_button(parent, "Swap", 4, toggle_cb, 0);
    make_button(parent, "RGB", 63, toggle_cb, 1);
    make_button(parent, "Invert", 122, toggle_cb, 2);
    lv_obj_t *apply = make_button(parent, "Apply", 181, apply_cb, 0);
    lv_obj_set_size(apply, 55, 34);
    lv_obj_set_style_bg_color(apply, lv_color_hex(COLOR_ACCENT_GREEN), 0);
}
