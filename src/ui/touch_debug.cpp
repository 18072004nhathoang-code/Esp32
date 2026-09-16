#include "touch_debug.h"
#include "ui_theme.h"
#include "fonts/ui_fonts.h"
#include "shared_i2c_bus.h"

static lv_obj_t *s_parent = nullptr;
static lv_obj_t *s_info = nullptr;
static lv_obj_t *s_cross_h = nullptr;
static lv_obj_t *s_cross_v = nullptr;
static lv_timer_t *s_timer = nullptr;

static const char *event_name(uint8_t event)
{
    switch (event)
    {
        case SHARED_TOUCH_EVENT_DOWN: return "DOWN";
        case SHARED_TOUCH_EVENT_UP: return "UP";
        case SHARED_TOUCH_EVENT_CONTACT: return "CONTACT";
        default: return "NONE";
    }
}

static void update_debug(lv_timer_t *)
{
    if (!s_parent || !s_info) return;
    SharedTouchSnapshot snap = {};
    if (!shared_i2c_touch_get_snapshot(&snap)) return;
    lv_label_set_text_fmt(s_info,
        "raw %u,%u   mapped %u,%u\n%s  id %u  points %u  seq %lu\nI2C %s   sample %s",
        snap.raw_x, snap.raw_y, snap.mapped_x, snap.mapped_y,
        event_name(snap.event), snap.touch_id, snap.point_count,
        static_cast<unsigned long>(snap.sequence), snap.io_ok ? "OK" : "ERROR",
        snap.sample_valid ? "VALID" : "INVALID");

    if (!snap.sample_valid || !snap.pressed)
    {
        lv_obj_add_flag(s_cross_h, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cross_v, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(s_parent, &area);
    const lv_coord_t local_x = static_cast<lv_coord_t>(snap.mapped_x) - area.x1;
    const lv_coord_t local_y = static_cast<lv_coord_t>(snap.mapped_y) - area.y1;
    if (local_x < 0 || local_y < 0 || local_x >= lv_obj_get_width(s_parent) || local_y >= lv_obj_get_height(s_parent))
        return;
    lv_obj_set_pos(s_cross_h, local_x - 8, local_y);
    lv_obj_set_pos(s_cross_v, local_x, local_y - 8);
    lv_obj_clear_flag(s_cross_h, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cross_v, LV_OBJ_FLAG_HIDDEN);
}

void ui_touch_debug_open(lv_obj_t *parent)
{
    ui_touch_debug_close();
    s_parent = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_OS_BG), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    s_info = lv_label_create(parent);
    lv_obj_set_width(s_info, lv_obj_get_width(parent) - 16);
    lv_obj_set_pos(s_info, 8, 8);
    lv_obj_set_style_text_font(s_info, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_info, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_label_set_text(s_info, "Chạm màn hình để xem dữ liệu FT6336");

    lv_obj_t *note = lv_label_create(parent);
    lv_obj_set_width(note, lv_obj_get_width(parent) - 16);
    lv_obj_set_pos(note, 8, 82);
    lv_label_set_text(note, "Quan sát thụ động • không hiệu chỉnh • tọa độ toàn màn hình");
    lv_obj_set_style_text_font(note, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    s_cross_h = lv_obj_create(parent);
    s_cross_v = lv_obj_create(parent);
    for (lv_obj_t *line : {s_cross_h, s_cross_v})
    {
        lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_ACCENT_CYAN), 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_set_style_pad_all(line, 0, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_size(s_cross_h, 17, 1);
    lv_obj_set_size(s_cross_v, 1, 17);
    s_timer = lv_timer_create(update_debug, 25, nullptr);
    update_debug(nullptr);
}

void ui_touch_debug_close(void)
{
    if (s_timer)
    {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    s_parent = nullptr;
    s_info = nullptr;
    s_cross_h = nullptr;
    s_cross_v = nullptr;
}
