#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "ble_settings_layout.h"
#include "ui_theme.h"

static void flush(lv_disp_drv_t *driver, const lv_area_t *, lv_color_t *) {
    lv_disp_flush_ready(driver);
}

static void check_gap(lv_obj_t *before, lv_obj_t *after) {
    lv_area_t a, b;
    lv_obj_get_coords(before, &a);
    lv_obj_get_coords(after, &b);
    if (b.y1 - a.y2 - 1 < 8)
        std::fprintf(stderr, "Layout collision: text bottom=%d, next top=%d, gap=%d\n",
                     a.y2, b.y1, b.y1 - a.y2 - 1);
    assert(b.y1 - a.y2 - 1 >= 8);
}

int main() {
    lv_init();
    static lv_color_t pixels[240 * 8];
    lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, pixels, nullptr, 240 * 8);
    lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = 240;
    driver.ver_res = 320;
    driver.draw_buf = &buffer;
    driver.flush_cb = flush;
    lv_disp_t *display = lv_disp_drv_register(&driver);
    assert(display);
    lv_disp_set_theme(display, lv_theme_default_init(display, lv_palette_main(LV_PALETTE_BLUE),
                      lv_palette_main(LV_PALETTE_RED), true, UI_FONT_BODY));
    // Same scrollable Settings content origin/width and card position as firmware.
    lv_obj_t *parent = lv_obj_create(lv_scr_act());
    assert(parent);
    lv_obj_set_size(parent, 240, APP_CONTENT_HEIGHT);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_t *card = lv_obj_create(parent);
    assert(card);
    lv_obj_t *title = lv_label_create(card);
    lv_obj_t *status = lv_label_create(card);
    lv_obj_t *hint = lv_label_create(card);
    lv_obj_t *button = lv_btn_create(card);
    assert(title && status && hint && button);
    lv_obj_t *toggle = lv_label_create(button);
    assert(toggle);
    lv_label_set_text(toggle, "Tắt Bluetooth BLE");
    lv_obj_set_style_text_font(toggle, UI_FONT_BUTTON, 0);
    lv_obj_center(toggle);
    lv_label_set_text(title, "Điều khiển nhạc qua BLE");
    lv_label_set_text(hint, "WiFi giữ nguyên. Không phải loa Bluetooth.");
    ble_settings_ui::layout(card, title, status, hint, button, 224);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 256);
    char maximum_error[96];
    for (unsigned i = 0; i < sizeof(maximum_error) - 1; ++i) maximum_error[i] = 'W';
    maximum_error[sizeof(maximum_error) - 1] = '\0';
    char maximum_status[sizeof(maximum_error) + 24];
    std::snprintf(maximum_status, sizeof(maximum_status), "MiniOS-B25A31\n%s", maximum_error);
    const char *states[] = {
        "MiniOS-B25A31\nĐã kết nối bảo mật",
        "MiniOS-B25A31\nGhép đôi trên điện thoại\nMã: 123456",
        "MiniOS-B25A31\nThiếu bộ nhớ. Dừng nhạc và đóng AI rồi bật lại.",
        maximum_status,
        "MiniOS-B25A31\nĐã tắt"
    };
    for (const char *state : states) {
        lv_label_set_text(status, state);
        lv_obj_update_layout(card);
        lv_obj_scroll_to_view_recursive(button, LV_ANIM_OFF);
        lv_obj_update_layout(parent);
        std::printf("card=%d status=%d hint=%d button=%d\n", lv_obj_get_height(card),
                    lv_obj_get_height(status), lv_obj_get_height(hint), lv_obj_get_height(button));
        check_gap(hint, button);
        check_gap(title, status);
        check_gap(status, hint);
        assert(lv_obj_get_height(button) == 44);
        assert(lv_obj_get_width(button) == lv_obj_get_content_width(card));
        assert(!lv_obj_has_flag(card, LV_OBJ_FLAG_SCROLLABLE));
        if (state == maximum_status) assert(lv_obj_get_height(card) > 224);
        lv_area_t content, bounds;
        lv_obj_get_content_coords(card, &content);
        for (lv_obj_t *child : {title, status, hint, button}) {
            lv_obj_get_coords(child, &bounds);
            assert(bounds.x1 >= content.x1 && bounds.x2 <= content.x2);
            assert(bounds.y1 >= content.y1 && bounds.y2 <= content.y2);
        }
        lv_obj_get_coords(button, &content);
        lv_obj_get_coords(toggle, &bounds);
        assert(bounds.x1 >= content.x1 && bounds.x2 <= content.x2);
        assert(bounds.y1 >= content.y1 && bounds.y2 <= content.y2);
        lv_obj_get_content_coords(parent, &content);
        lv_obj_get_coords(button, &bounds);
        assert(bounds.y1 >= content.y1 && bounds.y2 <= content.y2);
    }
    lv_obj_del(parent);
    std::puts("BLE Settings actual LVGL/Vietnamese font layout PASS");
}
