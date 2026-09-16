/**
 * @file ui_manager.cpp
 * @brief Giao diện hệ điều hành Mini OS responsive, tối ưu 240x320 portrait.
 * Phong cách TikTok / Modern Mobile OS: Dynamic Status Bar, Grid 3 cột, Floating Bottom Dock,
 * Card bo góc Squircle, Dark Mode Obsidian và chuyển cảnh mượt mà.
 */

#include "ui_manager.h"
#include "ui_theme.h"
#include "color_test.h"
#include "touch_debug.h"
#include "../display/lvgl_port.h"
#include "../apps/map_app.h"
#include "../apps/audio_app.h"
#include "../apps/wifi_app.h"
#include "../apps/music_app.h"
#include "../apps/ai_voice_app.h"
#include "../apps/camera_app.h"
#include "../audio/audio_manager.h"
#include "../audio/music_player.h"
#include "../ai/ai_voice_service.h"
#include "../os/wifi_manager.h"
#include "../os/power_manager.h"
#include "../os/settings_service.h"
#include "../storage/storage_manager.h"
#include "shared_i2c_bus.h"
#include "../camera/camera_service.h"

#ifndef FW_GIT_SHA
#define FW_GIT_SHA "unknown"
#endif

// Biến giao diện chính
static lv_obj_t *status_bar = nullptr;
static lv_obj_t *lbl_clock = nullptr;
static lv_obj_t *lbl_ram_pill = nullptr;
static lv_obj_t *lbl_wifi_icon = nullptr;
static lv_obj_t *lbl_spk_icon = nullptr;
static lv_obj_t *lbl_battery_pill = nullptr;

static lv_obj_t *desktop_view = nullptr;
static lv_obj_t *dock_bar = nullptr;
static lv_obj_t *app_window = nullptr;
static lv_obj_t *app_title_lbl = nullptr;
static lv_obj_t *app_content_container = nullptr;

// Widget của System Monitor App
static lv_obj_t *arc_cpu = nullptr;
static lv_obj_t *lbl_cpu_arc_val = nullptr;
static lv_obj_t *arc_ram = nullptr;
static lv_obj_t *lbl_ram_arc_val = nullptr;
static lv_obj_t *chart_system = nullptr;
static lv_chart_series_t *ser_cpu = nullptr;
static lv_chart_series_t *ser_ram = nullptr;
static lv_obj_t *lbl_temp_chip = nullptr;
static lv_obj_t *lbl_uptime_chip = nullptr;
static lv_obj_t *lbl_psram_chip = nullptr;
static lv_obj_t *lbl_runtime_chip = nullptr;

// Widget của Settings App
static lv_obj_t *slider_brightness = nullptr;
static lv_obj_t *lbl_brightness_val = nullptr;
static lv_obj_t *lbl_settings_status = nullptr;
static lv_obj_t *sw_wifi_reconnect = nullptr;

// Widget của Power Manager App
static lv_obj_t *lbl_power_state = nullptr;
static lv_obj_t *lbl_power_details = nullptr;
static lv_obj_t *lbl_power_timeouts = nullptr;

// Widget của Tools & Sensors App
static lv_obj_t *lbl_compass_val = nullptr;
static lv_obj_t *lbl_pitch_val = nullptr;

// Màu chủ đề Accent hiện tại (Mặc định: Cyan)
static lv_color_t theme_accent = lv_color_hex(COLOR_ACCENT_CYAN);

enum AppID : uintptr_t {
    APP_NONE = 0,
    APP_SYSTEM = 1,
    APP_SETTINGS = 2,
    APP_WIFI = 3,
    APP_ABOUT = 4,
    APP_MAP = 5,
    APP_TOOLS = 6,
    APP_AUDIO = 7,
    APP_MUSIC = 8,
    APP_AI_VOICE = 9,
    APP_CAMERA = 10,
    APP_POWER = 11,
    APP_COLOR_TEST = 13,
    APP_TOUCH_DEBUG = 14
};
static AppID active_app = APP_NONE;

// Khai báo trước các hàm mở app
static void open_system_monitor_app(void);
static void open_settings_app(void);
static void open_wifi_app(void);
static void open_tools_app(void);
static void open_about_app(void);
static void open_map_app(void);
static void open_audio_app(void);
static void open_music_app(void);
static void open_ai_voice_app(void);
static void open_camera_app(void);
static void open_power_app(void);
static void close_current_app(void);
static void prepare_app_window(const char *title, AppID app_id);

static void apply_accent_theme(void)
{
    if (dock_bar) lv_obj_set_style_border_color(dock_bar, theme_accent, 0);
    if (app_title_lbl) lv_obj_set_style_text_color(app_title_lbl, theme_accent, 0);
    if (lbl_wifi_icon && wifi_manager_is_connected())
        lv_obj_set_style_text_color(lbl_wifi_icon, theme_accent, 0);
}

/* Callback khi bấm nút đóng cửa sổ app */
static void close_btn_event_cb(lv_event_t *e)
{
    (void)e;
    close_current_app();
}

/* Callback mở app từ Desktop hoặc Dock */
static void app_icon_event_cb(lv_event_t *e)
{
    uintptr_t app_id = (uintptr_t)lv_event_get_user_data(e);
    switch (app_id)
    {
        case APP_SYSTEM:     open_system_monitor_app(); break;
        case APP_SETTINGS:   open_settings_app(); break;
        case APP_WIFI:       open_wifi_app(); break;
        case APP_ABOUT:      open_about_app(); break;
        case APP_MAP:        open_map_app(); break;
        case APP_TOOLS:      open_tools_app(); break;
        case APP_AUDIO:      open_audio_app(); break;
        case APP_MUSIC:      open_music_app(); break;
        case APP_AI_VOICE:   open_ai_voice_app(); break;
        case APP_CAMERA:     open_camera_app(); break;
        case APP_POWER:      open_power_app(); break;
        default: break;
    }
}

/* Callback thanh trượt độ sáng màn hình PWM */
static void brightness_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    lvgl_port_set_brightness((uint8_t)val);

    if (lbl_brightness_val)
    {
        lv_label_set_text_fmt(lbl_brightness_val, "Độ sáng PWM: %d%%", val);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED && lbl_settings_status)
    {
        bool ok = settings_service_set_brightness((uint8_t)val);
        lv_label_set_text(lbl_settings_status, ok ? "Đã lưu NVS" : settings_service_get_last_error());
        lv_obj_set_style_text_color(lbl_settings_status,
            lv_color_hex(ok ? COLOR_ACCENT_GREEN : COLOR_ACCENT_RED), 0);
    }
}

/* Callback chọn màu chủ đề */
static void theme_color_event_cb(lv_event_t *e)
{
    uintptr_t color_val = (uintptr_t)lv_event_get_user_data(e);
    theme_accent = lv_color_hex(color_val);
    apply_accent_theme();
    bool ok = settings_service_set_accent((uint32_t)color_val);
    if (lbl_settings_status)
    {
        lv_label_set_text(lbl_settings_status, ok ? "Đã lưu màu chủ đề" : settings_service_get_last_error());
        lv_obj_set_style_text_color(lbl_settings_status,
            lv_color_hex(ok ? COLOR_ACCENT_GREEN : COLOR_ACCENT_RED), 0);
    }
}

static void wifi_reconnect_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (settings_service_set_wifi_auto_reconnect(enabled))
    {
        wifi_manager_set_auto_reconnect(enabled);
        if (lbl_settings_status) lv_label_set_text(lbl_settings_status, "Đã lưu tự động kết nối WiFi");
    }
    else if (lbl_settings_status)
    {
        lv_label_set_text(lbl_settings_status, settings_service_get_last_error());
        lv_obj_set_style_text_color(lbl_settings_status, lv_color_hex(COLOR_ACCENT_RED), 0);
    }
}

static void power_timeout_cycle_cb(lv_event_t *e)
{
    (void)e;
    uint32_t dim = power_manager_get_dim_timeout();
    uint32_t new_dim = 30, new_sleep = 60;
    if (dim <= 30) { new_dim = 60; new_sleep = 120; }
    else if (dim <= 60) { new_dim = 120; new_sleep = 300; }
    power_manager_set_timeouts(new_dim, new_sleep);
    bool ok = settings_service_set_power_timeouts(new_dim, new_sleep);
    if (lbl_power_timeouts)
    {
        lv_label_set_text_fmt(lbl_power_timeouts, "%s Mờ: %lus • Tắt LCD: %lus",
                              ok ? "" : "Lỗi lưu •", (unsigned long)new_dim, (unsigned long)new_sleep);
    }
}

static void restart_btn_cb(lv_event_t *e)
{
    (void)e;
    ESP.restart();
}


/* Callback mở màn hình Color Test */
static void color_test_btn_cb(lv_event_t *e)
{
    (void)e;
    prepare_app_window("Display Diagnostic", APP_COLOR_TEST);
    ui_color_test_open(app_content_container);
}

static void touch_debug_btn_cb(lv_event_t *e)
{
    (void)e;
    prepare_app_window("Touch Diagnostic", APP_TOUCH_DEBUG);
    ui_touch_debug_open(app_content_container);
}

/* Callback bấm nút Ngủ Ngay trong Power App */
static void sleep_now_btn_cb(lv_event_t *e)
{
    power_manager_sleep();
}

/* =========================================================================
 * 1. THANH TRẠNG THÁI STATUS BAR (CAO 22px)
 * ========================================================================= */
static void create_status_bar(void)
{
    status_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(status_bar, SCREEN_WIDTH, STATUS_BAR_HEIGHT);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(COLOR_OS_BG), 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(status_bar, 6, 0);
    lv_obj_set_style_pad_ver(status_bar, 1, 0);

    // Bên trái: Giờ / Đồng hồ số
    lbl_clock = lv_label_create(status_bar);
    lv_label_set_text(lbl_clock, "00:00");
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_clock, UI_FONT_12, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_LEFT_MID, 4, 0);

    // Bên phải: Cụm chỉ số tối giản (Speaker khi phát, WiFi, Pin)
    lv_obj_t *right_cluster = lv_obj_create(status_bar);
    lv_obj_set_size(right_cluster, 76, 20);
    lv_obj_align(right_cluster, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_opa(right_cluster, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_cluster, 0, 0);
    lv_obj_set_style_pad_all(right_cluster, 0, 0);
    lv_obj_clear_flag(right_cluster, LV_OBJ_FLAG_SCROLLABLE);

    lbl_spk_icon = lv_label_create(right_cluster);
    lv_label_set_text(lbl_spk_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_spk_icon, lv_color_hex(COLOR_ACCENT_AMBER), 0);
    lv_obj_set_style_text_font(lbl_spk_icon, UI_FONT_12, 0);
    lv_obj_align(lbl_spk_icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(lbl_spk_icon, LV_OBJ_FLAG_HIDDEN);

    lbl_wifi_icon = lv_label_create(right_cluster);
    lv_label_set_text(lbl_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(lbl_wifi_icon, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_wifi_icon, UI_FONT_12, 0);
    lv_obj_align(lbl_wifi_icon, LV_ALIGN_RIGHT_MID, -24, 0);

    lbl_battery_pill = lv_label_create(right_cluster);
    BatteryInfo init_bat = system_get_battery_info();
    if (!init_bat.has_battery)
    {
        lv_obj_add_flag(lbl_battery_pill, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        if (!init_bat.is_calibrated)
        {
            lv_label_set_text(lbl_battery_pill, LV_SYMBOL_BATTERY_EMPTY);
            lv_obj_set_style_text_color(lbl_battery_pill, lv_color_hex(COLOR_ACCENT_AMBER), 0);
        }
        else
        {
            lv_label_set_text(lbl_battery_pill, LV_SYMBOL_BATTERY_FULL);
            lv_obj_set_style_text_color(lbl_battery_pill, lv_color_hex(COLOR_ACCENT_GREEN), 0);
        }
        lv_obj_set_style_text_font(lbl_battery_pill, UI_FONT_12, 0);
        lv_obj_align(lbl_battery_pill, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

/* =========================================================================
 * 2. TẠO APP ICON SQUIRCLE CHO GRID 3 CỘT PORTRAIT
 * ========================================================================= */
static void create_grid_app_icon(lv_obj_t *parent, const char *symbol, const char *title, lv_color_t accent, uintptr_t app_id, int col, int row)
{
    int col_width = SCREEN_WIDTH / 3;
    int x = col * col_width + (col_width - 66) / 2;
    int y = 6 + row * 64;

    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, 66, 62);
    lv_obj_set_pos(container, x, y);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Nút squircle icon (40x40)
    lv_obj_t *btn = lv_btn_create(container);
    lv_obj_set_size(btn, APP_ICON_BOX_SIZE, APP_ICON_BOX_SIZE);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(btn, APP_ICON_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(btn, accent, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 6, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_add_event_cb(btn, app_icon_event_cb, LV_EVENT_CLICKED, (void *)app_id);

    lv_obj_t *lbl_sym = lv_label_create(btn);
    lv_label_set_text(lbl_sym, symbol);
    lv_obj_set_style_text_color(lbl_sym, accent, 0);
    lv_obj_set_style_text_font(lbl_sym, UI_FONT_14, 0);
    lv_obj_center(lbl_sym);

    // Tên ứng dụng bên dưới icon (Font 12 rõ ràng, sắc nét)
    lv_obj_t *lbl_title = lv_label_create(container);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_title, UI_FONT_12, 0);
    lv_obj_align(lbl_title, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* =========================================================================
 * 3. TẠO ICON CHO FLOATING BOTTOM DOCK
 * ========================================================================= */
static void create_dock_icon(lv_obj_t *parent, const char *symbol, lv_color_t accent, uintptr_t app_id, int x_pos)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, DOCK_ICON_BOX_SIZE, DOCK_ICON_BOX_SIZE);
    lv_obj_set_pos(btn, x_pos, 4);
    lv_obj_set_style_radius(btn, DOCK_ICON_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(btn, accent, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 4, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_add_event_cb(btn, app_icon_event_cb, LV_EVENT_CLICKED, (void *)app_id);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, accent, 0);
    lv_obj_set_style_text_font(lbl, UI_FONT_14, 0);
    lv_obj_center(lbl);
}

/* =========================================================================
 * 4. TẠO MÀN HÌNH HOME PORTRAIT VÀ DOCK NỔI
 * ========================================================================= */
static void create_desktop(void)
{
    desktop_view = lv_obj_create(lv_scr_act());
    lv_obj_set_size(desktop_view, SCREEN_WIDTH, SCREEN_HEIGHT - STATUS_BAR_HEIGHT);
    lv_obj_align(desktop_view, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(desktop_view, 0, 0);
    lv_obj_set_style_border_width(desktop_view, 0, 0);
    lv_obj_set_style_bg_color(desktop_view, lv_color_hex(COLOR_OS_BG), 0);
    lv_obj_clear_flag(desktop_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(desktop_view, 0, 0);

    // Grid 3 cột x 3 hàng; dock luôn nằm dưới và không chồng nội dung.
    create_grid_app_icon(desktop_view, LV_SYMBOL_CHARGE,   "System",    lv_color_hex(COLOR_ACCENT_CYAN),   APP_SYSTEM,     0, 0);
    create_grid_app_icon(desktop_view, LV_SYMBOL_AUDIO,    "AI Voice",  lv_color_hex(COLOR_ACCENT_CYAN),   APP_AI_VOICE,   1, 0);
    create_grid_app_icon(desktop_view, LV_SYMBOL_PLAY,     "Voice Lab", lv_color_hex(COLOR_ACCENT_BLUE),   APP_AUDIO,      2, 0);
    create_grid_app_icon(desktop_view, LV_SYMBOL_SETTINGS, "Settings",  lv_color_hex(COLOR_ACCENT_AMBER),  APP_SETTINGS,   0, 1);
    create_grid_app_icon(desktop_view, LV_SYMBOL_POWER,    "Power",     lv_color_hex(COLOR_ACCENT_GREEN),  APP_POWER,      1, 1);
    create_grid_app_icon(desktop_view, LV_SYMBOL_EYE_OPEN, "Sensors",   lv_color_hex(COLOR_ACCENT_PURPLE), APP_TOOLS,      2, 1);
    create_grid_app_icon(desktop_view, LV_SYMBOL_LIST,     "About",     lv_color_hex(COLOR_TEXT_SECONDARY),APP_ABOUT,      0, 2);

    // 5. FLOATING BOTTOM DOCK (Chứa 4 app hay dùng: WiFi, Music, Maps, Camera)
    dock_bar = lv_obj_create(desktop_view);
    const int dock_width = SCREEN_WIDTH - 12;
    lv_obj_set_size(dock_bar, dock_width, 44);
    lv_obj_align(dock_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(dock_bar, 22, 0);
    lv_obj_set_style_bg_color(dock_bar, lv_color_hex(COLOR_DOCK_BG), 0);
    lv_obj_set_style_bg_opa(dock_bar, LV_OPA_90, 0);
    lv_obj_set_style_border_color(dock_bar, lv_color_hex(COLOR_DOCK_BORDER), 0);
    lv_obj_set_style_border_width(dock_bar, 1, 0);
    lv_obj_set_style_pad_all(dock_bar, 0, 0);
    lv_obj_clear_flag(dock_bar, LV_OBJ_FLAG_SCROLLABLE);

    const int dock_gap = (dock_width - 4 * DOCK_ICON_BOX_SIZE) / 5;
    create_dock_icon(dock_bar, LV_SYMBOL_WIFI,  lv_color_hex(COLOR_ACCENT_GREEN),  APP_WIFI,   dock_gap);
    create_dock_icon(dock_bar, LV_SYMBOL_AUDIO, lv_color_hex(COLOR_ACCENT_PURPLE), APP_MUSIC,  dock_gap * 2 + DOCK_ICON_BOX_SIZE);
    create_dock_icon(dock_bar, LV_SYMBOL_GPS,   lv_color_hex(COLOR_ACCENT_RED),    APP_MAP,    dock_gap * 3 + DOCK_ICON_BOX_SIZE * 2);
    create_dock_icon(dock_bar, LV_SYMBOL_IMAGE, lv_color_hex(0xFF006E),            APP_CAMERA, dock_gap * 4 + DOCK_ICON_BOX_SIZE * 3);
}

/* =========================================================================
 * 5. KHUNG CỬA SỔ ỨNG DỤNG RESPONSIVE
 * ========================================================================= */
static void ensure_app_window(void)
{
    if (app_window != nullptr) return;

    app_window = lv_obj_create(lv_scr_act());
    lv_obj_set_size(app_window, SCREEN_WIDTH, SCREEN_HEIGHT - STATUS_BAR_HEIGHT);
    lv_obj_align(app_window, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(app_window, 0, 0);
    lv_obj_set_style_border_width(app_window, 0, 0);
    lv_obj_set_style_bg_color(app_window, lv_color_hex(COLOR_OS_BG), 0);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(app_window, 0, 0);

    // Thanh tiêu đề App
    lv_obj_t *header = lv_obj_create(app_window);
    lv_obj_set_size(header, SCREEN_WIDTH, APP_HEADER_HEIGHT);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(COLOR_HEADER_BG), 0);
    lv_obj_set_style_border_color(header, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    app_title_lbl = lv_label_create(header);
    lv_label_set_text(app_title_lbl, "App");
    lv_obj_align(app_title_lbl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(app_title_lbl, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(app_title_lbl, UI_FONT_TITLE, 0);
    lv_obj_set_width(app_title_lbl, SCREEN_WIDTH - 58);
    lv_label_set_long_mode(app_title_lbl, LV_LABEL_LONG_DOT);

    // Nút đóng app (X) tối thiểu >=32x32 hit area
    lv_obj_t *close_btn = lv_btn_create(header);
    lv_obj_set_size(close_btn, 34, 28);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_radius(close_btn, 6, 0);
    lv_obj_set_ext_click_area(close_btn, 6); // Hit area 46x34 >= 32x32
    lv_obj_add_event_cb(close_btn, close_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_lbl, UI_FONT_12, 0);
    lv_obj_center(close_lbl);

    // Khung chứa nội dung ứng dụng, tính từ logical screen và header 30 px.
    app_content_container = lv_obj_create(app_window);
    lv_obj_set_size(app_content_container, SCREEN_WIDTH, APP_CONTENT_HEIGHT);
    lv_obj_align(app_content_container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(app_content_container, lv_color_hex(COLOR_OS_BG), 0);
    lv_obj_set_style_border_width(app_content_container, 0, 0);
    lv_obj_set_style_pad_all(app_content_container, 0, 0);

    lv_obj_add_flag(app_window, LV_OBJ_FLAG_HIDDEN);
}

static void invalidate_active_app_widgets(void)
{
    switch (active_app)
    {
        case APP_MAP: map_app_close(); break;
        case APP_AUDIO: audio_app_close(); break;
        case APP_WIFI: wifi_app_close(); break;
        case APP_MUSIC: music_app_close(); break;
        case APP_AI_VOICE: ai_voice_app_close(); break;
        case APP_CAMERA: camera_app_close(); break;
        case APP_COLOR_TEST: ui_color_test_close(); break;
        case APP_TOUCH_DEBUG: ui_touch_debug_close(); break;
        default: break;
    }

    arc_cpu = nullptr;
    lbl_cpu_arc_val = nullptr;
    arc_ram = nullptr;
    lbl_ram_arc_val = nullptr;
    chart_system = nullptr;
    ser_cpu = nullptr;
    ser_ram = nullptr;
    lbl_temp_chip = nullptr;
    lbl_uptime_chip = nullptr;
    lbl_psram_chip = nullptr;
    lbl_runtime_chip = nullptr;
    slider_brightness = nullptr;
    lbl_brightness_val = nullptr;
    lbl_settings_status = nullptr;
    sw_wifi_reconnect = nullptr;
    lbl_power_state = nullptr;
    lbl_power_details = nullptr;
    lbl_power_timeouts = nullptr;
    lbl_compass_val = nullptr;
    lbl_pitch_val = nullptr;
    active_app = APP_NONE;
}

static void prepare_app_window(const char *title, AppID app_id)
{
    ensure_app_window();
    invalidate_active_app_widgets();
    lv_obj_clean(app_content_container);
    lv_label_set_text(app_title_lbl, title);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);
    active_app = app_id;
}

static void close_current_app(void)
{
    invalidate_active_app_widgets();
    if (app_content_container) lv_obj_clean(app_content_container);
    if (app_window) lv_obj_add_flag(app_window, LV_OBJ_FLAG_HIDDEN);
    if (desktop_view) lv_obj_clear_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
}

/* =========================================================================
 * 6. ỨNG DỤNG SYSTEM MONITOR: DUAL SPEEDOMETER + REALTIME CHART
 * ========================================================================= */
static void open_system_monitor_app(void)
{
    prepare_app_window("System Monitor", APP_SYSTEM);
    SystemStats initial = system_get_stats();
    lv_obj_add_flag(app_content_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(app_content_container, 6, 0);

    // Card 1: Dual Speedometers (CPU + RAM)
    lv_obj_t *card_gauges = lv_obj_create(app_content_container);
    lv_obj_set_size(card_gauges, SCREEN_WIDTH - 12, 80);
    lv_obj_align(card_gauges, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(card_gauges, 10, 0);
    lv_obj_set_style_bg_color(card_gauges, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card_gauges, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(card_gauges, 1, 0);
    lv_obj_set_style_pad_all(card_gauges, 4, 0);
    lv_obj_clear_flag(card_gauges, LV_OBJ_FLAG_SCROLLABLE);

    // CPU Arc
    arc_cpu = lv_arc_create(card_gauges);
    lv_obj_set_size(arc_cpu, 60, 60);
    lv_obj_align(arc_cpu, LV_ALIGN_LEFT_MID, 20, 0);
    lv_arc_set_rotation(arc_cpu, 135);
    lv_arc_set_bg_angles(arc_cpu, 0, 270);
    lv_arc_set_range(arc_cpu, 0, 100);
    lv_arc_set_value(arc_cpu, initial.cpu_usage_available ? initial.cpu_usage_percent : 0);
    lv_obj_set_style_arc_color(arc_cpu, lv_color_hex(COLOR_ACCENT_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_cpu, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_cpu, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_cpu, 5, LV_PART_MAIN);
    lv_obj_clear_flag(arc_cpu, LV_OBJ_FLAG_CLICKABLE);

    lbl_cpu_arc_val = lv_label_create(arc_cpu);
    if (initial.cpu_usage_available)
        lv_label_set_text_fmt(lbl_cpu_arc_val, "%u%%\nCPU", initial.cpu_usage_percent);
    else
        lv_label_set_text(lbl_cpu_arc_val, "--\nCPU");
    lv_obj_set_style_text_align(lbl_cpu_arc_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_cpu_arc_val, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_cpu_arc_val, UI_FONT_SMALL, 0);
    lv_obj_center(lbl_cpu_arc_val);

    // RAM Arc
    arc_ram = lv_arc_create(card_gauges);
    lv_obj_set_size(arc_ram, 60, 60);
    lv_obj_align(arc_ram, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_arc_set_rotation(arc_ram, 135);
    lv_arc_set_bg_angles(arc_ram, 0, 270);
    lv_arc_set_range(arc_ram, 0, 100);
    lv_arc_set_value(arc_ram, initial.heap_usage_percent);
    lv_obj_set_style_arc_color(arc_ram, lv_color_hex(COLOR_ACCENT_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_ram, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_ram, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_ram, 5, LV_PART_MAIN);
    lv_obj_clear_flag(arc_ram, LV_OBJ_FLAG_CLICKABLE);

    lbl_ram_arc_val = lv_label_create(arc_ram);
    lv_label_set_text_fmt(lbl_ram_arc_val, "%u%%\nRAM", initial.heap_usage_percent);
    lv_obj_set_style_text_align(lbl_ram_arc_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_ram_arc_val, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_ram_arc_val, UI_FONT_SMALL, 0);
    lv_obj_center(lbl_ram_arc_val);

    // Card 2: Live Chart
    chart_system = lv_chart_create(app_content_container);
    lv_obj_set_size(chart_system, SCREEN_WIDTH - 12, 85);
    lv_obj_align(chart_system, LV_ALIGN_TOP_MID, 0, 86);
    lv_chart_set_type(chart_system, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_system, 24);
    lv_chart_set_range(chart_system, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart_system, 3, 5);
    lv_obj_set_style_bg_color(chart_system, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(chart_system, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(chart_system, 1, 0);
    lv_obj_set_style_radius(chart_system, 8, 0);

    ser_cpu = lv_chart_add_series(chart_system, lv_color_hex(COLOR_ACCENT_CYAN), LV_CHART_AXIS_PRIMARY_Y);
    ser_ram = lv_chart_add_series(chart_system, lv_color_hex(COLOR_ACCENT_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart_system, ser_cpu, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart_system, ser_ram, LV_CHART_POINT_NONE);

    // Card 3: Telemetry & Memory Box
    lv_obj_t *telemetry_box = lv_obj_create(app_content_container);
    lv_obj_set_size(telemetry_box, SCREEN_WIDTH - 12, 112);
    lv_obj_align(telemetry_box, LV_ALIGN_TOP_MID, 0, 178);
    lv_obj_set_style_radius(telemetry_box, 8, 0);
    lv_obj_set_style_bg_color(telemetry_box, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(telemetry_box, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(telemetry_box, 1, 0);
    lv_obj_set_style_pad_all(telemetry_box, 6, 0);
    lv_obj_clear_flag(telemetry_box, LV_OBJ_FLAG_SCROLLABLE);

    lbl_temp_chip = lv_label_create(telemetry_box);
    lv_label_set_text_fmt(lbl_temp_chip, "Nhiệt độ chip: %.1f °C", initial.core_temp_c);
    lv_obj_set_style_text_color(lbl_temp_chip, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_text_font(lbl_temp_chip, UI_FONT_SMALL, 0);
    lv_obj_set_pos(lbl_temp_chip, 4, 4);

    lbl_psram_chip = lv_label_create(telemetry_box);
    lv_label_set_text_fmt(lbl_psram_chip, "PSRAM: %.1f / %.1f MB",
                          (float)initial.used_psram / 1048576.0f,
                          (float)initial.total_psram / 1048576.0f);
    lv_obj_set_style_text_color(lbl_psram_chip, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_text_font(lbl_psram_chip, UI_FONT_SMALL, 0);
    lv_obj_set_pos(lbl_psram_chip, 4, 26);

    lbl_uptime_chip = lv_label_create(telemetry_box);
    lv_label_set_text_fmt(lbl_uptime_chip, LV_SYMBOL_REFRESH " Uptime: %s", initial.uptime_str);
    lv_obj_set_style_text_color(lbl_uptime_chip, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_uptime_chip, UI_FONT_SMALL, 0);
    lv_obj_set_pos(lbl_uptime_chip, 4, 48);

    lbl_runtime_chip = lv_label_create(telemetry_box);
    lv_label_set_text_fmt(lbl_runtime_chip, "Task: %lu • SD: %s • WiFi: %s",
                          (unsigned long)initial.task_count,
                          initial.storage_available ? "OK" : "N/A",
                          initial.wifi_connected ? "OK" : "OFF");
    lv_obj_set_style_text_color(lbl_runtime_chip, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_runtime_chip, UI_FONT_SMALL, 0);
    lv_obj_set_pos(lbl_runtime_chip, 4, 70);
}

/* =========================================================================
 * 7. ỨNG DỤNG SETTINGS CONTROL CENTER (CUỘN DỌC)
 * ========================================================================= */
static void open_settings_app(void)
{
    prepare_app_window("Settings", APP_SETTINGS);
    MiniOsSettings cfg = settings_service_get();
    theme_accent = lv_color_hex(cfg.accent_rgb);
    apply_accent_theme();
    lv_obj_add_flag(app_content_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(app_content_container, 8, 0);

    // Card 1: Độ sáng màn hình
    lv_obj_t *card_bright = lv_obj_create(app_content_container);
    lv_obj_set_size(card_bright, SCREEN_WIDTH - 16, 75);
    lv_obj_align(card_bright, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(card_bright, 10, 0);
    lv_obj_set_style_bg_color(card_bright, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card_bright, lv_color_hex(COLOR_ACCENT_AMBER), 0);
    lv_obj_set_style_border_width(card_bright, 1, 0);
    lv_obj_clear_flag(card_bright, LV_OBJ_FLAG_SCROLLABLE);

    lbl_brightness_val = lv_label_create(card_bright);
    lv_label_set_text_fmt(lbl_brightness_val, "Độ sáng Màn hình: %u%%", cfg.brightness);
    lv_obj_set_style_text_color(lbl_brightness_val, lv_color_hex(COLOR_ACCENT_AMBER), 0);
    lv_obj_set_style_text_font(lbl_brightness_val, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_brightness_val, LV_ALIGN_TOP_LEFT, 0, 0);

    slider_brightness = lv_slider_create(card_bright);
    lv_obj_set_size(slider_brightness, SCREEN_WIDTH - 44, 14);
    lv_obj_align(slider_brightness, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_slider_set_range(slider_brightness, 10, 100);
    lv_slider_set_value(slider_brightness, cfg.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(COLOR_ACCENT_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(0xFFD54F), LV_PART_KNOB);
    lv_obj_add_event_cb(slider_brightness, brightness_slider_event_cb, LV_EVENT_ALL, nullptr);

    // Card 2: Màu chủ đề Accent
    lv_obj_t *card_theme = lv_obj_create(app_content_container);
    lv_obj_set_size(card_theme, SCREEN_WIDTH - 16, 75);
    lv_obj_align(card_theme, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_radius(card_theme, 10, 0);
    lv_obj_set_style_bg_color(card_theme, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card_theme, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(card_theme, 1, 0);
    lv_obj_clear_flag(card_theme, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *theme_title = lv_label_create(card_theme);
    lv_label_set_text(theme_title, "Màu Chủ Đề Accent:");
    lv_obj_set_style_text_color(theme_title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(theme_title, UI_FONT_SMALL, 0);
    lv_obj_align(theme_title, LV_ALIGN_TOP_LEFT, 0, 0);

    int pill_w = (SCREEN_WIDTH - 48) / 4;
    auto create_color_pill = [&](int idx, uint32_t hex, const char *txt) {
        lv_obj_t *b = lv_btn_create(card_theme);
        lv_obj_set_size(b, pill_w, 26);
        lv_obj_set_pos(b, 2 + idx * (pill_w + 4), 22);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(hex), 0);
        lv_obj_add_event_cb(b, theme_color_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)hex);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(0x0A0D14), 0);
        lv_obj_set_style_text_font(l, UI_FONT_SMALL, 0);
        lv_obj_center(l);
    };

    create_color_pill(0, COLOR_ACCENT_CYAN, "Cyan");
    create_color_pill(1, COLOR_ACCENT_GREEN, "Grn");
    create_color_pill(2, COLOR_ACCENT_RED, "Red");
    create_color_pill(3, COLOR_ACCENT_PURPLE, "Purp");

    // Card 3: cấu hình WiFi thật, lưu NVS
    lv_obj_t *card_pwr = lv_obj_create(app_content_container);
    lv_obj_set_size(card_pwr, SCREEN_WIDTH - 16, 54);
    lv_obj_align(card_pwr, LV_ALIGN_TOP_MID, 0, 164);
    lv_obj_set_style_radius(card_pwr, 10, 0);
    lv_obj_set_style_bg_color(card_pwr, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card_pwr, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_border_width(card_pwr, 1, 0);
    lv_obj_clear_flag(card_pwr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_pwr_t = lv_label_create(card_pwr);
    lv_label_set_text(lbl_pwr_t, "WiFi tự kết nối lại");
    lv_obj_set_style_text_color(lbl_pwr_t, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_text_font(lbl_pwr_t, UI_FONT_12, 0);
    lv_obj_align(lbl_pwr_t, LV_ALIGN_LEFT_MID, 0, 0);

    sw_wifi_reconnect = lv_switch_create(card_pwr);
    lv_obj_set_size(sw_wifi_reconnect, 48, 26);
    lv_obj_align(sw_wifi_reconnect, LV_ALIGN_RIGHT_MID, 0, 0);
    if (cfg.wifi_auto_reconnect) lv_obj_add_state(sw_wifi_reconnect, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_wifi_reconnect, wifi_reconnect_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Card 4: Chẩn đoán màu màn hình
    lv_obj_t *card_diag = lv_obj_create(app_content_container);
    lv_obj_set_size(card_diag, SCREEN_WIDTH - 16, 75);
    lv_obj_align(card_diag, LV_ALIGN_TOP_MID, 0, 226);
    lv_obj_set_style_radius(card_diag, 10, 0);
    lv_obj_set_style_bg_color(card_diag, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card_diag, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_border_width(card_diag, 1, 0);
    lv_obj_clear_flag(card_diag, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_diag_t = lv_label_create(card_diag);
    lv_label_set_text(lbl_diag_t, "Chẩn Đoán Màn Hình");
    lv_obj_set_style_text_color(lbl_diag_t, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_text_font(lbl_diag_t, UI_FONT_12, 0);
    lv_obj_align(lbl_diag_t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *btn_color_test = lv_btn_create(card_diag);
    lv_obj_set_size(btn_color_test, SCREEN_WIDTH - 32, 34);
    lv_obj_align(btn_color_test, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_radius(btn_color_test, 6, 0);
    lv_obj_set_style_bg_color(btn_color_test, lv_color_hex(0x1F2A38), 0);
    lv_obj_set_style_border_color(btn_color_test, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_width(btn_color_test, 1, 0);
    lv_obj_add_event_cb(btn_color_test, color_test_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_ct = lv_label_create(btn_color_test);
    lv_label_set_text(lbl_ct, LV_SYMBOL_IMAGE " Test Màu");
    lv_obj_set_style_text_color(lbl_ct, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_text_font(lbl_ct, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_ct);

    lbl_settings_status = lv_label_create(app_content_container);
    lv_label_set_text(lbl_settings_status, "Cấu hình được lưu trong NVS");
    lv_obj_set_style_text_color(lbl_settings_status, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_settings_status, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_settings_status, LV_ALIGN_TOP_LEFT, 8, 306);
}

/* =========================================================================
 * 8. ỨNG DỤNG POWER MANAGER
 * ========================================================================= */
static void open_power_app(void)
{
    prepare_app_window("Power Manager", APP_POWER);
    BatteryInfo battery = system_get_battery_info();
    lv_obj_add_flag(app_content_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(app_content_container, 8, 0);

    // Card 1: Trạng thái nguồn hiện tại
    lv_obj_t *card_stat = lv_obj_create(app_content_container);
    lv_obj_set_size(card_stat, SCREEN_WIDTH - 16, 92);
    lv_obj_align(card_stat, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(card_stat, 10, 0);
    lv_obj_set_style_bg_color(card_stat, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card_stat, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_border_width(card_stat, 1, 0);
    lv_obj_clear_flag(card_stat, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t_pwr = lv_label_create(card_stat);
    lv_label_set_text(t_pwr, "Trạng Thái Nguồn");
    lv_obj_set_style_text_color(t_pwr, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_text_font(t_pwr, UI_FONT_SMALL, 0);
    lv_obj_align(t_pwr, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_power_state = lv_label_create(card_stat);
    lv_label_set_text(lbl_power_state, LV_SYMBOL_OK " Hoạt Động (100%)");
    lv_obj_set_style_text_color(lbl_power_state, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_power_state, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_power_state, LV_ALIGN_BOTTOM_LEFT, 0, -4);

    lbl_power_details = lv_label_create(card_stat);
    lv_label_set_text_fmt(lbl_power_details, "Pin: %s • Sạc: không có driver",
                          battery.has_battery ? battery.status_str : "không hỗ trợ");
    lv_obj_set_width(lbl_power_details, SCREEN_WIDTH - 38);
    lv_label_set_long_mode(lbl_power_details, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl_power_details, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_power_details, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_power_details, LV_ALIGN_CENTER, 0, 6);

    // Card 2: Hẹn giờ tự động mờ và ngủ
    lv_obj_t *card_timer = lv_obj_create(app_content_container);
    lv_obj_set_size(card_timer, SCREEN_WIDTH - 16, 90);
    lv_obj_align(card_timer, LV_ALIGN_TOP_MID, 0, 99);
    lv_obj_set_style_radius(card_timer, 10, 0);
    lv_obj_set_style_bg_color(card_timer, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card_timer, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(card_timer, 1, 0);
    lv_obj_clear_flag(card_timer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t_t = lv_label_create(card_timer);
    lv_label_set_text(t_t, "Thời Gian Chờ:");
    lv_obj_set_style_text_color(t_t, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(t_t, UI_FONT_SMALL, 0);
    lv_obj_align(t_t, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_power_timeouts = lv_label_create(card_timer);
    lv_label_set_text_fmt(lbl_power_timeouts, "Mờ: %lus • Tắt LCD: %lus",
                          (unsigned long)power_manager_get_dim_timeout(),
                          (unsigned long)power_manager_get_sleep_timeout());
    lv_obj_set_style_text_color(lbl_power_timeouts, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_power_timeouts, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_power_timeouts, LV_ALIGN_LEFT_MID, 0, 8);

    lv_obj_t *btn_timeout = lv_btn_create(card_timer);
    lv_obj_set_size(btn_timeout, 62, 30);
    lv_obj_align(btn_timeout, LV_ALIGN_RIGHT_MID, 0, 8);
    lv_obj_add_event_cb(btn_timeout, power_timeout_cycle_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl_timeout = lv_label_create(btn_timeout);
    lv_label_set_text(lbl_timeout, "Đổi");
    lv_obj_set_style_text_font(lbl_timeout, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_timeout);

    // Card 3: Thao tác Ngủ Ngay (Sleep Now)
    lv_obj_t *btn_sleep = lv_btn_create(app_content_container);
    lv_obj_set_size(btn_sleep, SCREEN_WIDTH - 16, 36);
    lv_obj_align(btn_sleep, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_set_style_radius(btn_sleep, 8, 0);
    lv_obj_set_style_bg_color(btn_sleep, lv_color_hex(0x281A22), 0);
    lv_obj_set_style_border_color(btn_sleep, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_border_width(btn_sleep, 1, 0);
    lv_obj_add_event_cb(btn_sleep, sleep_now_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_slp = lv_label_create(btn_sleep);
    lv_label_set_text(lbl_slp, LV_SYMBOL_POWER " Tắt Màn Hình & Ngủ Ngay");
    lv_obj_set_style_text_color(lbl_slp, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_text_font(lbl_slp, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_slp);

    lv_obj_t *btn_restart = lv_btn_create(app_content_container);
    lv_obj_set_size(btn_restart, SCREEN_WIDTH - 16, 36);
    lv_obj_align(btn_restart, LV_ALIGN_TOP_MID, 0, 238);
    lv_obj_set_style_radius(btn_restart, 8, 0);
    lv_obj_set_style_bg_color(btn_restart, lv_color_hex(0x1F2937), 0);
    lv_obj_add_event_cb(btn_restart, restart_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl_restart = lv_label_create(btn_restart);
    lv_label_set_text(lbl_restart, LV_SYMBOL_REFRESH " Khởi động lại ESP32");
    lv_obj_set_style_text_font(lbl_restart, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_restart);
}

/* =========================================================================
 * 9. ỨNG DỤNG SENSORS & TOOLS
 * ========================================================================= */
static void open_tools_app(void)
{
    prepare_app_window("Sensors & Diagnostics", APP_TOOLS);
    lv_obj_set_style_pad_all(app_content_container, 8, 0);

    lv_obj_t *compass_card = lv_obj_create(app_content_container);
    lv_obj_set_size(compass_card, SCREEN_WIDTH - 16, APP_CONTENT_HEIGHT - 16);
    lv_obj_center(compass_card);
    lv_obj_set_style_radius(compass_card, 10, 0);
    lv_obj_set_style_bg_color(compass_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(compass_card, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_width(compass_card, 1, 0);

    lv_obj_t *t = lv_label_create(compass_card);
    lv_label_set_text(t, "Hardware Sensors");
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_text_font(t, UI_FONT_12, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 2);

    lbl_compass_val = lv_label_create(compass_card);
    lv_label_set_text(lbl_compass_val,
        "IMU / La bàn: không khả dụng\nÁp suất: không khả dụng\nBoard không khai báo driver cảm biến.");
    lv_obj_set_style_text_color(lbl_compass_val, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_compass_val, UI_FONT_12, 0);
    lv_obj_set_width(lbl_compass_val, SCREEN_WIDTH - 40);
    lv_label_set_long_mode(lbl_compass_val, LV_LABEL_LONG_WRAP);
    lv_obj_align(lbl_compass_val, LV_ALIGN_TOP_LEFT, 0, 34);

    lbl_pitch_val = lv_label_create(compass_card);
    lv_label_set_text_fmt(lbl_pitch_val,
        "Touch FT6336: %s\nAudio ES8311: %s\nMicroSD: %s\nCamera DVP: %s",
        shared_i2c_touch_is_detected() ? "OK" : "không phát hiện",
        shared_i2c_codec_is_detected() ? "OK" : "không phát hiện",
        storage_is_available() ? "OK" : "không phát hiện",
        camera_feature_status_to_string(camera_service_get_local_dvp_status()));
    lv_obj_set_style_text_color(lbl_pitch_val, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_pitch_val, UI_FONT_12, 0);
    lv_obj_align(lbl_pitch_val, LV_ALIGN_TOP_LEFT, 0, 104);

    // Nút mở Color Self-Test
    lv_obj_t *btn_ct = lv_btn_create(compass_card);
    lv_obj_set_size(btn_ct, (SCREEN_WIDTH - 42) / 2, 34);
    lv_obj_align(btn_ct, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_set_style_bg_color(btn_ct, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_color(btn_ct, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_width(btn_ct, 1, 0);
    lv_obj_set_style_radius(btn_ct, 6, 0);
    lv_obj_set_ext_click_area(btn_ct, 4);
    lv_obj_add_event_cb(btn_ct, color_test_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_ct = lv_label_create(btn_ct);
    lv_label_set_text(lbl_ct, LV_SYMBOL_IMAGE " Màu");
    lv_obj_set_style_text_color(lbl_ct, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_text_font(lbl_ct, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_ct);

    lv_obj_t *btn_touch = lv_btn_create(compass_card);
    lv_obj_set_size(btn_touch, (SCREEN_WIDTH - 42) / 2, 34);
    lv_obj_align(btn_touch, LV_ALIGN_BOTTOM_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(btn_touch, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_color(btn_touch, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_border_width(btn_touch, 1, 0);
    lv_obj_set_style_radius(btn_touch, 6, 0);
    lv_obj_add_event_cb(btn_touch, touch_debug_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl_touch = lv_label_create(btn_touch);
    lv_label_set_text(lbl_touch, "Touch");
    lv_obj_set_style_text_font(lbl_touch, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_touch);
}

/* =========================================================================
 * 10. ỨNG DỤNG ABOUT MINI OS
 * ========================================================================= */
static void open_about_app(void)
{
    prepare_app_window("About Mini OS", APP_ABOUT);
    lv_obj_set_style_pad_all(app_content_container, 8, 0);

    lv_obj_t *card = lv_obj_create(app_content_container);
    lv_obj_set_size(card, SCREEN_WIDTH - 16, APP_CONTENT_HEIGHT - 16);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_border_width(card, 1, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "ESP32-S3 Mini OS");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *desc = lv_label_create(card);
    BatteryInfo bat = system_get_battery_info();
    SystemStats stats = system_get_stats();
    lv_label_set_text_fmt(desc,
        "Commit: %.12s\n"
        "Board: %s\n"
        "LCD: %dx%d • rotation %d (%s)\n"
        "LVGL: %d.%d.%d\n"
        "MCU: %s @ %uMHz\n"
        "Flash: %uMB • PSRAM: %uMB\n"
        "Pin: %s\n"
        "Audio: %s • Touch: %s",
        FW_GIT_SHA, BOARD_PROFILE_NAME,
        DISP_HOR_RES, DISP_VER_RES, BOARD_LCD_ROTATION,
        display_orientation_name(BOARD_LCD_ROTATION),
        LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
        ESP.getChipModel(), stats.cpu_freq_mhz,
        stats.flash_size_mb, stats.total_psram / 1048576U,
        bat.status_str,
        audio_is_driver_installed() ? "OK" : "DEGRADED",
        shared_i2c_touch_is_detected() ? "OK" : "MISSING");
    lv_obj_set_width(desc, SCREEN_WIDTH - 40);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(desc, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(desc, UI_FONT_12, 0);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, 10);
}

/* =========================================================================
 * CÁC HÀM MỞ APP TỪ CORE KHÁC HOẶC DESKTOP
 * ========================================================================= */
static void open_map_app(void)
{
    prepare_app_window("Network Map", APP_MAP);
    map_app_open(app_content_container);
}

static void open_audio_app(void)
{
    prepare_app_window("Audio & Voice Lab", APP_AUDIO);
    audio_app_open(app_content_container);
}

static void open_wifi_app(void)
{
    prepare_app_window("WiFi Settings", APP_WIFI);
    wifi_app_open(app_content_container);
}

void ui_open_wifi_app(void)
{
    if (lvgl_port_lock(500))
    {
        open_wifi_app();
        lvgl_port_unlock();
    }
}

static void open_music_app(void)
{
    prepare_app_window("Music Player", APP_MUSIC);
    music_app_open(app_content_container);
}

void ui_open_music_app(void)
{
    if (lvgl_port_lock(500))
    {
        open_music_app();
        lvgl_port_unlock();
    }
}

static void open_ai_voice_app(void)
{
    prepare_app_window("XiaoZhi AI Voice", APP_AI_VOICE);
    ai_voice_app_open(app_content_container);
}

void ui_open_ai_voice_app(void)
{
    if (lvgl_port_lock(500))
    {
        open_ai_voice_app();
        lvgl_port_unlock();
    }
}

static void open_camera_app(void)
{
    prepare_app_window("IP Camera", APP_CAMERA);
    camera_app_open(app_content_container);
}

void ui_open_camera_app(void)
{
    if (lvgl_port_lock(500))
    {
        open_camera_app();
        lvgl_port_unlock();
    }
}

/* =========================================================================
 * KHỞI TẠO HỆ THỐNG GIAO DIỆN
 * ========================================================================= */
void ui_init(void)
{
    theme_accent = lv_color_hex(settings_service_get().accent_rgb);
    if (lvgl_port_lock(1000))
    {
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_OS_BG), 0);
        create_status_bar();
        create_desktop();
        apply_accent_theme();
        lvgl_port_unlock();
    }
}

/* =========================================================================
 * CẬP NHẬT ĐỊNH KỲ THỜI GIAN THỰC
 * ========================================================================= */
void ui_update_periodic(const SystemStats &stats)
{
    if (!lvgl_port_lock(200)) return;

    // 1. Cập nhật đồng hồ Status Bar
    if (lbl_clock)
    {
        uint32_t s = stats.uptime_sec;
        lv_label_set_text_fmt(lbl_clock, "%02u:%02u", (s % 3600) / 60, s % 60);
    }

    // Cập nhật biểu tượng Loa (chỉ hiện khi đang phát âm thanh)
    if (lbl_spk_icon)
    {
        bool is_audio_active = (audio_get_current_owner() == AUDIO_OWNER_MUSIC || audio_is_playing());
        if (is_audio_active)
            lv_obj_clear_flag(lbl_spk_icon, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(lbl_spk_icon, LV_OBJ_FLAG_HIDDEN);
    }

    // 3. Cập nhật WiFi Icon
    if (lbl_wifi_icon)
    {
        lv_obj_set_style_text_color(lbl_wifi_icon,
            wifi_manager_is_connected() ? lv_color_hex(COLOR_ACCENT_GREEN) : lv_color_hex(COLOR_TEXT_MUTED), 0);
    }

    // 4. Cập nhật System Monitor (nếu đang mở)
    if (arc_cpu && lbl_cpu_arc_val)
    {
        const int cpu = stats.cpu_usage_available ? stats.cpu_usage_percent : 0;
        lv_arc_set_value(arc_cpu, cpu);
        if (stats.cpu_usage_available) lv_label_set_text_fmt(lbl_cpu_arc_val, "%d%%\nCPU", cpu);
        else lv_label_set_text(lbl_cpu_arc_val, "--\nCPU");

        if (chart_system && ser_cpu)
        {
            lv_chart_set_next_value(chart_system, ser_cpu,
                                    stats.cpu_usage_available ? cpu : LV_CHART_POINT_NONE);
        }
    }

    if (arc_ram && lbl_ram_arc_val)
    {
        lv_arc_set_value(arc_ram, stats.heap_usage_percent);
        lv_label_set_text_fmt(lbl_ram_arc_val, "%d%%\nRAM", stats.heap_usage_percent);

        if (chart_system && ser_ram)
        {
            lv_chart_set_next_value(chart_system, ser_ram, stats.heap_usage_percent);
        }
    }

    if (lbl_temp_chip)
    {
        lv_label_set_text_fmt(lbl_temp_chip, "Nhiệt độ: %.1f °C", stats.core_temp_c);
    }

    if (lbl_psram_chip)
    {
        lv_label_set_text_fmt(lbl_psram_chip, "PSRAM: %.1f / %.1f MB",
                              (float)stats.used_psram / 1048576.0f,
                              (float)stats.total_psram / 1048576.0f);
    }

    if (lbl_uptime_chip)
    {
        lv_label_set_text_fmt(lbl_uptime_chip, LV_SYMBOL_REFRESH " Uptime: %s", stats.uptime_str);
    }

    if (lbl_runtime_chip)
    {
        if (stats.storage_available)
        {
            lv_label_set_text_fmt(lbl_runtime_chip, "Task: %lu • SD: %llu/%lluMB • WiFi: %ddBm",
                                  (unsigned long)stats.task_count,
                                  stats.storage_free_mb, stats.storage_total_mb,
                                  stats.wifi_rssi);
        }
        else
        {
            lv_label_set_text_fmt(lbl_runtime_chip, "Task: %lu • SD: N/A • WiFi: %s",
                                  (unsigned long)stats.task_count,
                                  stats.wifi_connected ? "OK" : "OFF");
        }
    }

    // 5. Cập nhật Power State (nếu đang mở)
    if (lbl_power_state)
    {
        PowerState p_st = power_manager_get_state();
        if (p_st == POWER_STATE_ACTIVE)
            lv_label_set_text(lbl_power_state, LV_SYMBOL_OK " Hoạt Động (100%)");
        else if (p_st == POWER_STATE_DIMMED)
            lv_label_set_text(lbl_power_state, LV_SYMBOL_EYE_CLOSE " Mờ Màn Hình (20%)");
        else
            lv_label_set_text(lbl_power_state, LV_SYMBOL_POWER " Tắt Màn Hình");
    }

    if (lbl_power_details)
    {
        BatteryInfo battery = system_get_battery_info();
        lv_label_set_text_fmt(lbl_power_details, "Pin: %s • Sạc: không có driver",
                              battery.has_battery ? battery.status_str : "không hỗ trợ");
    }

    // 6. Cập nhật icon Loa trên Status Bar
    if (lbl_spk_icon)
    {
        bool pa_on = (audio_get_volume() > 0 && audio_is_pa_enabled());
        lv_obj_set_style_text_color(lbl_spk_icon, pa_on ? lv_color_hex(COLOR_ACCENT_AMBER) : lv_color_hex(COLOR_TEXT_MUTED), 0);
    }

    // 6b. Cập nhật icon Pin thực tế trên Status Bar
    if (lbl_battery_pill)
    {
        BatteryInfo bat = system_get_battery_info();
        if (!bat.has_battery)
        {
            lv_obj_add_flag(lbl_battery_pill, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_clear_flag(lbl_battery_pill, LV_OBJ_FLAG_HIDDEN);
            if (!bat.is_calibrated)
            {
                lv_label_set_text(lbl_battery_pill, LV_SYMBOL_BATTERY_EMPTY);
                lv_obj_set_style_text_color(lbl_battery_pill, lv_color_hex(COLOR_ACCENT_AMBER), 0);
            }
            else
            {
                if (bat.percentage >= 80)
                    lv_label_set_text(lbl_battery_pill, LV_SYMBOL_BATTERY_FULL);
                else if (bat.percentage >= 50)
                    lv_label_set_text(lbl_battery_pill, LV_SYMBOL_BATTERY_3);
                else if (bat.percentage >= 20)
                    lv_label_set_text(lbl_battery_pill, LV_SYMBOL_BATTERY_2);
                else
                    lv_label_set_text(lbl_battery_pill, LV_SYMBOL_BATTERY_EMPTY);
                lv_obj_set_style_text_color(lbl_battery_pill, lv_color_hex(COLOR_ACCENT_GREEN), 0);
            }
        }
    }

    // 7. Cập nhật các app con
    audio_app_update();
    wifi_app_update();
    map_app_render();
    music_app_update();
    ai_voice_app_update();
    camera_app_update();

    lvgl_port_unlock();
}
