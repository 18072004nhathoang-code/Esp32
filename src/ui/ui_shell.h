#pragma once

#include "ui_theme.h"
#include "home_icons.h"

// Production LVGL shell, also exercised by the native layout tests.
// Only view construction lives here: callbacks keep the existing app/services.
namespace minios_shell {
struct App { const char *symbol; const char *title; uint32_t accent; uintptr_t id; };
struct Home { lv_obj_t *root; lv_obj_t *scroll; lv_obj_t *dock; };

inline void flat(lv_obj_t *obj, uint32_t color, lv_coord_t radius = 0) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

inline void hint_event(lv_event_t *e) {
    lv_obj_t *button=lv_event_get_target(e);
    lv_obj_t *icon=lv_obj_get_child(button,0), *label=lv_obj_get_child(button,1);
    if(lv_event_get_code(e)==LV_EVENT_LONG_PRESSED) {
        lv_obj_add_flag(icon,LV_OBJ_FLAG_HIDDEN);lv_obj_clear_flag(label,LV_OBJ_FLAG_HIDDEN);
    } else if(lv_event_get_code(e)==LV_EVENT_RELEASED || lv_event_get_code(e)==LV_EVENT_PRESS_LOST) {
        lv_obj_clear_flag(icon,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(label,LV_OBJ_FLAG_HIDDEN);
    }
}
inline lv_obj_t *launcher(lv_obj_t *parent, const App &app, lv_coord_t w,
                          lv_coord_t h, lv_event_cb_t callback, bool dock) {
    lv_obj_t *button = lv_btn_create(parent);
    (void)dock;
    const uint32_t colors[]={0x536079,0x52647C,0x576A83,0x0875DB,0x52647C,0x159765,0x52647C,0x52647C,0xCE315A,0x6651C8,0x52647C,0xB76400};
    flat(button, app.id<12?colors[app.id]:0x52647C, 14);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, reinterpret_cast<void *>(app.id));
    lv_obj_add_event_cb(button,hint_event,LV_EVENT_ALL,nullptr);
    lv_obj_t *icon = lv_img_create(button);
    lv_img_set_src(icon,home_icons::get(app.id));
    lv_obj_set_style_img_recolor(icon,lv_color_white(),0);
    lv_obj_set_style_img_recolor_opa(icon,LV_OPA_COVER,0);
    lv_obj_center(icon);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, app.title);
    lv_obj_set_width(label, w - 4);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, UI_FONT_12, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_flag(label,LV_OBJ_FLAG_HIDDEN);
    return button;
}

inline Home create_home(lv_obj_t *screen, const App *apps, unsigned count,
                        const App (&shortcuts)[3], lv_event_cb_t callback) {
    Home home = {};
    home.root = lv_obj_create(screen);
    flat(home.root, COLOR_OS_BG);
    lv_obj_set_size(home.root, SCREEN_WIDTH, SCREEN_HEIGHT - STATUS_BAR_HEIGHT);
    lv_obj_align(home.root, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_grad_color(home.root,lv_color_hex(0x253653),0);
    lv_obj_set_style_bg_grad_dir(home.root,LV_GRAD_DIR_VER,0);

    home.scroll = lv_obj_create(home.root);
    flat(home.scroll, COLOR_OS_BG);
    lv_obj_set_style_bg_opa(home.scroll,LV_OPA_TRANSP,0);
    lv_obj_set_pos(home.scroll, 0, 18);
    lv_obj_set_size(home.scroll, SCREEN_WIDTH, SCREEN_HEIGHT - STATUS_BAR_HEIGHT - 26 - DOCK_HEIGHT);
    lv_obj_add_flag(home.scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(home.scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(home.scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(home.scroll, 8, 0);
    for (unsigned i = 0; i < count; ++i) {
        lv_obj_t *button = launcher(home.scroll, apps[i], 52, 52, callback, false);
        lv_obj_set_pos(button, 14 + (i % 3) * 80, (i / 3) * 76);
    }
    home.dock = lv_obj_create(home.root);
    flat(home.dock, COLOR_DOCK_BG,22);
    lv_obj_set_size(home.dock, SCREEN_WIDTH-16, DOCK_HEIGHT);
    lv_obj_align(home.dock, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_border_width(home.dock, 1, 0);
    lv_obj_set_style_border_side(home.dock, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(home.dock, lv_color_hex(COLOR_DOCK_BORDER), 0);
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *button = launcher(home.dock, shortcuts[i], 52, 52, callback, true);
        lv_obj_set_pos(button, 8 + i * 78, 7);
    }
    return home;
}

inline lv_obj_t *create_header(lv_obj_t *window, lv_obj_t **title, lv_event_cb_t on_close) {
    lv_obj_t *header = lv_obj_create(window);
    flat(header, COLOR_HEADER_BG);
    lv_obj_set_size(header, SCREEN_WIDTH, APP_HEADER_HEIGHT);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    *title = lv_label_create(header);
    lv_obj_set_style_text_font(*title, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(*title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_width(*title, SCREEN_WIDTH - 68);
    lv_label_set_long_mode(*title, LV_LABEL_LONG_DOT);
    lv_obj_align(*title, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *close = lv_btn_create(header);
    flat(close, COLOR_HEADER_BG, 14);
    lv_obj_set_size(close, 44, 44);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(COLOR_CARD_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = lv_label_create(close);
    lv_label_set_text(label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_text_font(label, UI_FONT_16, 0);
    lv_obj_center(label);
    return header;
}
} // namespace minios_shell
