#pragma once

#include <lvgl.h>
#include "fonts/ui_fonts.h"

namespace ble_settings_ui {
// Shared by the Settings UI and the native test using pinned LVGL + real fonts.
inline void layout(lv_obj_t *card, lv_obj_t *title, lv_obj_t *status,
                   lv_obj_t *hint, lv_obj_t *button, lv_coord_t width) {
    lv_obj_set_size(card, width, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 224, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // Content can grow when the Vietnamese status/error/passkey wraps. Never
    // anchor the button over text or create a nested scroll region.
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title, UI_FONT_BODY, 0);
    lv_obj_set_width(status, LV_PCT(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(status, UI_FONT_BODY, 0);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, UI_FONT_12, 0);
    lv_obj_set_size(button, LV_PCT(100), 44);
}
} // namespace ble_settings_ui
