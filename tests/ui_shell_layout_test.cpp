#include <cassert>
#include <cstdio>
#include <cstring>
#include "ui_shell.h"
#include "ui_app_catalog.h"
#include "navigation_view.h"

static unsigned opened = 0;
static uintptr_t last_id = 0;
static void click(lv_event_t *event) {
    ++opened;
    last_id = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
}
static void flush(lv_disp_drv_t *driver, const lv_area_t *, lv_color_t *) {
    lv_disp_flush_ready(driver);
}
static void inside(lv_obj_t *child, lv_obj_t *parent) {
    lv_area_t a, b;
    lv_obj_get_coords(child, &a);
    lv_obj_get_content_coords(parent, &b);
    assert(a.x1 >= b.x1 && a.x2 <= b.x2);
    assert(a.y1 >= b.y1 && a.y2 <= b.y2);
}
int main() {
    static_assert(SCREEN_WIDTH == 240 && SCREEN_HEIGHT == 320, "ES3C28P layout");
    static_assert(BOARD_LCD_ROTATION == 2, "Portrait Flipped preserved");
    static_assert(APP_CONTENT_HEIGHT == 238, "44px header and 16px gesture strip reserved");
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
    lv_disp_set_theme(display, lv_theme_default_init(display, lv_color_hex(COLOR_ACCENT_BLUE),
                      lv_color_hex(COLOR_ACCENT_PURPLE), true, UI_FONT_BODY));
    minios_shell::App apps[11];
    for (unsigned i = 0; i < 11; ++i)
        apps[i] = {LV_SYMBOL_AUDIO, i == 2 ? "Voice Lab" : "Ứng dụng", COLOR_ACCENT_BLUE, i + 1};
    const minios_shell::App dock[3] = {
        {LV_SYMBOL_AUDIO, "Music", COLOR_ACCENT_PURPLE, 12},
        {LV_SYMBOL_AUDIO, "AI Voice", COLOR_ACCENT_BLUE, 13},
        {LV_SYMBOL_SETTINGS, "Settings", COLOR_TEXT_SECONDARY, 14}
    };
    const minios_shell::Home home = minios_shell::create_home(lv_scr_act(), apps, 11, dock, click);
    lv_obj_update_layout(home.root);
    assert(lv_obj_get_child_cnt(home.scroll) == 11);
    assert(lv_obj_get_child_cnt(home.dock) == 3);
    lv_area_t scroll, dock_area;
    lv_obj_get_coords(home.scroll, &scroll);
    lv_obj_get_coords(home.dock, &dock_area);
    assert(scroll.y2 < dock_area.y1);
    assert(lv_obj_get_scroll_bottom(home.scroll) > 0);
    for (unsigned i = 0; i < 11; ++i) {
        lv_obj_t *button = lv_obj_get_child(home.scroll, i);
        assert(lv_obj_get_width(button) >= 44 && lv_obj_get_height(button) >= 44);
        lv_obj_scroll_to_view(button, LV_ANIM_OFF);
        lv_obj_update_layout(home.root);
        inside(button, home.scroll);
        inside(lv_obj_get_child(button, 1), button);
        lv_event_send(button, LV_EVENT_SHORT_CLICKED, nullptr);
        assert(last_id == i + 1);
    }
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *button = lv_obj_get_child(home.dock, i);
        assert(lv_obj_get_height(button) >= 44);
        inside(button, home.dock);
        inside(lv_obj_get_child(button, 1), button);
        lv_event_send(button, LV_EVENT_SHORT_CLICKED, nullptr);
        assert(last_id == i + 12);
    }
    assert(opened == 14);
    lv_obj_del(home.root);
    // Test the actual production catalog, not only the long synthetic grid.
    const auto &visible = minios_catalog::home_apps();
    const auto &shortcuts = minios_catalog::dock_apps();
    const uintptr_t expected[] = {APP_SYSTEM, APP_WIFI, APP_MAP, APP_CAMERA, APP_POWER};
    const uintptr_t dock_expected[] = {APP_MUSIC, APP_AI_VOICE, APP_SETTINGS};
    const uintptr_t hidden[] = {APP_AUDIO, APP_TOOLS, APP_HEALTH, APP_ABOUT,
                               APP_COLOR_TEST, APP_TOUCH_DEBUG};
    const minios_shell::Home compact = minios_shell::create_home(lv_scr_act(), visible,
        sizeof(visible) / sizeof(visible[0]), shortcuts, click);
    lv_obj_update_layout(compact.root);
    assert(lv_obj_get_child_cnt(compact.scroll) == 5);
    assert(lv_obj_get_child_cnt(compact.dock) == 3);
    assert(lv_obj_get_child_cnt(compact.root)==2); // no Home heading/subtitle
    // LVGL returns negative spare room when content is shorter than the viewport.
    assert(lv_obj_get_scroll_bottom(compact.scroll) <= 0);
    for (unsigned i = 0; i < 5; ++i) {
        assert(visible[i].id == expected[i]);
        for (uintptr_t id : hidden) assert(visible[i].id != id);
        lv_obj_t *button = lv_obj_get_child(compact.scroll, i);
        inside(button, compact.scroll);
        assert(std::strcmp(lv_label_get_text(lv_obj_get_child(button, 1)),
                           visible[i].title) == 0);
        assert(lv_obj_has_flag(lv_obj_get_child(button,1),LV_OBJ_FLAG_HIDDEN));
        const unsigned before=opened;
        lv_event_send(button,LV_EVENT_LONG_PRESSED,nullptr);
        assert(!lv_obj_has_flag(lv_obj_get_child(button,1),LV_OBJ_FLAG_HIDDEN));
        lv_event_send(button,LV_EVENT_RELEASED,nullptr);
        assert(opened==before);
        lv_event_send(button, LV_EVENT_SHORT_CLICKED, nullptr);
        assert(last_id == expected[i]);
    }
    for (unsigned i = 0; i < 3; ++i) {
        assert(shortcuts[i].id == dock_expected[i]);
        for (uintptr_t id : hidden) assert(shortcuts[i].id != id);
        lv_obj_t *button = lv_obj_get_child(compact.dock, i);
        inside(button, compact.dock);
        lv_event_send(button, LV_EVENT_SHORT_CLICKED, nullptr);
        assert(last_id == dock_expected[i]);
    }
    assert(opened == 22);
    lv_obj_del(compact.root);
    lv_obj_t *window = lv_obj_create(lv_scr_act());
    minios_shell::flat(window, COLOR_OS_BG);
    lv_obj_set_size(window, SCREEN_WIDTH, SCREEN_HEIGHT - STATUS_BAR_HEIGHT);
    lv_obj_align(window, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *title = nullptr;
    lv_obj_t *header = minios_shell::create_header(window, &title, click);
    lv_label_set_text(title, "Touch Diagnostic");
    lv_obj_update_layout(window);
    lv_obj_t *close = lv_obj_get_child(header, 1);
    assert(lv_obj_get_width(close) == 44 && lv_obj_get_height(close) == 44);
    inside(close, header);
    lv_area_t title_area, close_area;
    lv_obj_get_coords(title, &title_area);
    lv_obj_get_coords(close, &close_area);
    assert(title_area.x2 < close_area.x1);
    lv_event_send(close, LV_EVENT_CLICKED, nullptr);
    assert(opened == 23);
    lv_obj_del(window);
    navigation_ui::View nav;
    lv_obj_t *parent=lv_obj_create(lv_scr_act());minios_shell::flat(parent,COLOR_OS_BG);lv_obj_set_size(parent,240,APP_CONTENT_HEIGHT);
    navigation_ui::open(nav,parent,click);
    NavigationSnapshot snapshot;snapshot.active=snapshot.fresh=true;snapshot.guidance.meters=200;snapshot.guidance.maneuver=2;
    strcpy(snapshot.guidance.instruction,"Rẽ trái vào đường Nguyễn Văn Linh");
    navigation_ui::update(nav,snapshot);lv_obj_update_layout(parent);
    for(unsigned i=0;i<lv_obj_get_child_cnt(nav.root);++i)inside(lv_obj_get_child(nav.root,i),nav.root);
    lv_refr_now(display);
    snapshot.fresh=false;navigation_ui::update(nav,snapshot);assert(nav.maneuver==0);
    lv_obj_del(parent);
    std::puts("iPhone-style production LVGL shell / fonts / scroll / callbacks PASS");
}
