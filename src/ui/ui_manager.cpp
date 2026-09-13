/**
 * @file ui_manager.cpp
 * @brief Giao diện hệ điều hành Mini OS PRO MAX: Dynamic Status Bar, Glassmorphism Desktop,
 * Dual-Arc Speedometer System Dashboard, Real-time Live Chart, WiFi Connectivity Hub & Control Center.
 */

#include "ui_manager.h"
#include "../display/lvgl_port.h"
#include "../apps/map_app.h"
#include "../apps/audio_app.h"
#include "../apps/wifi_app.h"
#include "../apps/music_app.h"
#include "../apps/ai_voice_app.h"
#include "../audio/audio_manager.h"
#include "../audio/music_player.h"
#include "../ai/ai_voice_service.h"
#include "../os/wifi_manager.h"
#include "../camera/camera_service.h"

// Biến giao diện chính
static lv_obj_t *status_bar = nullptr;
static lv_obj_t *lbl_clock = nullptr;
static lv_obj_t *lbl_ram_pill = nullptr;
static lv_obj_t *lbl_wifi_icon = nullptr;
static lv_obj_t *lbl_spk_icon = nullptr;
static lv_obj_t *lbl_battery_pill = nullptr;
static lv_obj_t *desktop_view = nullptr;
static lv_obj_t *app_window = nullptr;
static lv_obj_t *app_title_lbl = nullptr;
static lv_obj_t *app_content_container = nullptr;

// Widget của System Monitor App PRO MAX
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

// Widget của Settings App PRO MAX
static lv_obj_t *slider_brightness = nullptr;
static lv_obj_t *lbl_brightness_val = nullptr;

// Widget của WiFi App PRO MAX
static lv_obj_t *wifi_status_lbl = nullptr;
static lv_obj_t *wifi_list = nullptr;
static lv_obj_t *wifi_pwd_modal = nullptr;
static lv_obj_t *wifi_ta_pass = nullptr;
static lv_obj_t *wifi_keyboard = nullptr;
static char selected_ssid[33] = {0};

// Widget của Tools & Sensors App
static lv_obj_t *lbl_compass_val = nullptr;
static lv_obj_t *lbl_pitch_val = nullptr;

// Màu chủ đề Accent hiện tại (Mặc định: Cyan Pro Max)
static lv_color_t theme_accent = lv_color_hex(0x00F2FE);

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
static void close_current_app(void);

/* Callback khi bấm nút đóng cửa sổ app */
static void close_btn_event_cb(lv_event_t *e)
{
    close_current_app();
}

/* Callback mở app từ Desktop */
static void app_icon_event_cb(lv_event_t *e)
{
    uintptr_t app_id = (uintptr_t)lv_event_get_user_data(e);
    switch (app_id)
    {
        case 1: open_system_monitor_app(); break;
        case 2: open_settings_app(); break;
        case 3: open_wifi_app(); break;
        case 4: open_about_app(); break;
        case 5: open_map_app(); break;
        case 6: open_tools_app(); break;
        case 7: open_audio_app(); break;
        case 8: open_music_app(); break;
        case 9: open_ai_voice_app(); break;
        case 10: open_camera_app(); break;
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
}

/* Callback chọn màu chủ đề */
static void theme_color_event_cb(lv_event_t *e)
{
    uintptr_t color_val = (uintptr_t)lv_event_get_user_data(e);
    theme_accent = lv_color_hex(color_val);
}

/* 1. TẠO THANH TRẠNG THÁI TOP DYNAMIC STATUS BAR (480x26) */
static void create_status_bar(void)
{
    status_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(status_bar, 480, 26);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x0A0D14), 0); // Obsidian Dark
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(status_bar, 10, 0);
    lv_obj_set_style_pad_ver(status_bar, 2, 0);

    // Bên trái: Huy hiệu phát sáng "● S3 3.5\" PRO MAX"
    lv_obj_t *lbl_os = lv_label_create(status_bar);
    lv_label_set_text(lbl_os, "● S3 3.5\" PRO MAX");
    lv_obj_set_style_text_color(lbl_os, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_text_font(lbl_os, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_os, LV_ALIGN_LEFT_MID, 0, 0);

    // Trung tâm: Đồng hồ số kỹ thuật số trắng sáng
    lbl_clock = lv_label_create(status_bar);
    lv_label_set_text(lbl_clock, "00:00:00");
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_clock, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, 0);

    // Bên phải: Cụm thông tin phần cứng cao cấp (Battery 100% ⚡, Speaker, WiFi 4-Bar, RAM Pill)
    lv_obj_t *right_cluster = lv_obj_create(status_bar);
    lv_obj_set_size(right_cluster, 145, 22);
    lv_obj_align(right_cluster, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(right_cluster, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_cluster, 0, 0);
    lv_obj_set_style_pad_all(right_cluster, 0, 0);
    lv_obj_clear_flag(right_cluster, LV_OBJ_FLAG_SCROLLABLE);

    // RAM Pill
    lbl_ram_pill = lv_label_create(right_cluster);
    lv_label_set_text(lbl_ram_pill, "28%");
    lv_obj_set_style_text_color(lbl_ram_pill, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_text_font(lbl_ram_pill, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_ram_pill, LV_ALIGN_LEFT_MID, 0, 0);

    // WiFi Icon
    lbl_wifi_icon = lv_label_create(right_cluster);
    lv_label_set_text(lbl_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(lbl_wifi_icon, lv_color_hex(0x718096), 0);
    lv_obj_set_style_text_font(lbl_wifi_icon, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_wifi_icon, LV_ALIGN_LEFT_MID, 36, 0);

    // Speaker Icon (Loa ngoài)
    lbl_spk_icon = lv_label_create(right_cluster);
    lv_label_set_text(lbl_spk_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_spk_icon, lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_text_font(lbl_spk_icon, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_spk_icon, LV_ALIGN_LEFT_MID, 60, 0);

    // Pin 100% ⚡
    lbl_battery_pill = lv_label_create(right_cluster);
    lv_label_set_text(lbl_battery_pill, LV_SYMBOL_BATTERY_FULL "⚡");
    lv_obj_set_style_text_color(lbl_battery_pill, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_text_font(lbl_battery_pill, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_battery_pill, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* 2. TẠO THẺ APP SQUIRCLE GLASSMORPHISM (140x120) */
static void create_app_squircle(lv_obj_t *parent, const char *symbol, const char *title, const char *subtitle, lv_color_t accent, uintptr_t app_id, int x, int y)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 140, 120);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 16, 0); // iOS Squircle Curve
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(btn, accent, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 12, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_50, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(btn, app_icon_event_cb, LV_EVENT_CLICKED, (void *)app_id);

    // Icon Biểu tượng ứng dụng với vòng nền gradient mờ
    lv_obj_t *icon_box = lv_obj_create(btn);
    lv_obj_set_size(icon_box, 42, 42);
    lv_obj_align(icon_box, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(icon_box, 12, 0);
    lv_obj_set_style_bg_color(icon_box, accent, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_20, 0);
    lv_obj_set_style_border_color(icon_box, accent, 0);
    lv_obj_set_style_border_width(icon_box, 1, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_icon = lv_label_create(icon_box);
    lv_label_set_text(lbl_icon, symbol);
    lv_obj_set_style_text_color(lbl_icon, accent, 0);
    lv_obj_set_style_text_font(lbl_icon, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_icon);

    // Tên Ứng dụng chính
    lv_obj_t *lbl_name = lv_label_create(btn);
    lv_label_set_text(lbl_name, title);
    lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_name, LV_ALIGN_BOTTOM_LEFT, 0, -18);

    // Phụ đề nhỏ (Subtitle / Status)
    lv_obj_t *lbl_sub = lv_label_create(btn);
    lv_label_set_text(lbl_sub, subtitle);
    lv_obj_set_style_text_color(lbl_sub, lv_color_hex(0x718096), 0);
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* 3. TẠO MÀN HÌNH DESKTOP LAUNCHER PRO MAX (LƯỚI 3 CỘT x 2 HÀNG CHO 3.5" IPS) */
static void create_desktop(void)
{
    desktop_view = lv_obj_create(lv_scr_act());
    lv_obj_set_size(desktop_view, 480, 294);
    lv_obj_align(desktop_view, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(desktop_view, 0, 0);
    lv_obj_set_style_border_width(desktop_view, 0, 0);
    lv_obj_set_style_bg_color(desktop_view, lv_color_hex(0x0A0D14), 0);
    
    // Cho phép cuộn dọc mượt mà trên màn hình 3.5" IPS
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(desktop_view, 0, 0);
    lv_obj_set_style_pad_bottom(desktop_view, 16, 0);

    // Hàng 1 (y = 16)
    create_app_squircle(desktop_view, LV_SYMBOL_CHARGE,   "System",        "Dual Core 240M",  lv_color_hex(0x00F2FE), 1, 15, 16);
    create_app_squircle(desktop_view, LV_SYMBOL_GPS,      "Maps Pro",      "WGS84 Vector",    lv_color_hex(0xFF3B30), 5, 170, 16);
    create_app_squircle(desktop_view, LV_SYMBOL_AUDIO,    "Music Player",  "SD Card MP3",     lv_color_hex(0x9D4EDD), 8, 325, 16);

    // Hàng 2 (y = 150)
    create_app_squircle(desktop_view, LV_SYMBOL_PLAY,     "Voice & Mic",   "XiaoZhi AI",      lv_color_hex(0x00F2FE), 7, 15, 150);
    create_app_squircle(desktop_view, LV_SYMBOL_WIFI,     "WiFi Hub",      "2.4GHz Scanner",  lv_color_hex(0x00E676), 3, 170, 150);
    create_app_squircle(desktop_view, LV_SYMBOL_SETTINGS, "Settings",      "Control Center",  lv_color_hex(0xFFB300), 2, 325, 150);

    // Hàng 3 (y = 284)
    create_app_squircle(desktop_view, LV_SYMBOL_EYE_OPEN, "Sensors",       "Compass & Info",  lv_color_hex(0x3A86FF), 6, 15, 284);
    create_app_squircle(desktop_view, LV_SYMBOL_AUDIO,    "AI Voice",      "XiaoZhi Gemini",  lv_color_hex(0x00F2FE), 9, 170, 284);
    create_app_squircle(desktop_view, LV_SYMBOL_LIST,     "About",         "Mini OS v2.5",    lv_color_hex(0x9D4EDD), 4, 325, 284);

    // Hàng 4 (y = 418)
    create_app_squircle(desktop_view, LV_SYMBOL_IMAGE,    "Camera",        "DVP / RTSP",      lv_color_hex(0xFF006E), 10, 15, 418);
}

/* 4. KHUNG CỬA SỔ ỨNG DỤNG PRO MAX (MODAL WINDOW 480x294) */
static void ensure_app_window(void)
{
    if (app_window != nullptr) return;

    app_window = lv_obj_create(lv_scr_act());
    lv_obj_set_size(app_window, 480, 294);
    lv_obj_align(app_window, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(app_window, 0, 0);
    lv_obj_set_style_border_width(app_window, 0, 0);
    lv_obj_set_style_bg_color(app_window, lv_color_hex(0x0A0D14), 0);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(app_window, 0, 0);

    // Thanh tiêu đề App Kính mờ
    lv_obj_t *header = lv_obj_create(app_window);
    lv_obj_set_size(header, 480, 28);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x121824), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    app_title_lbl = lv_label_create(header);
    lv_label_set_text(app_title_lbl, "App");
    lv_obj_align(app_title_lbl, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_text_color(app_title_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(app_title_lbl, &lv_font_montserrat_12, 0);

    // Nút đóng app (X) tròn bo tròn đẹp mắt
    lv_obj_t *close_btn = lv_btn_create(header);
    lv_obj_set_size(close_btn, 28, 20);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_add_event_cb(close_btn, close_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(close_lbl);

    // Khung chứa nội dung ứng dụng
    app_content_container = lv_obj_create(app_window);
    lv_obj_set_size(app_content_container, 480, 266);
    lv_obj_align(app_content_container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(app_content_container, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_border_width(app_content_container, 0, 0);
    lv_obj_set_style_pad_all(app_content_container, 0, 0);

    lv_obj_add_flag(app_window, LV_OBJ_FLAG_HIDDEN);
}

static void close_current_app(void)
{
    if (app_window)
    {
        lv_obj_add_flag(app_window, LV_OBJ_FLAG_HIDDEN);
    }
    if (desktop_view)
    {
        lv_obj_clear_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    }
    arc_cpu = nullptr;
    arc_ram = nullptr;
    chart_system = nullptr;
    ser_cpu = nullptr;
    ser_ram = nullptr;
    slider_brightness = nullptr;
    wifi_list = nullptr;
    wifi_pwd_modal = nullptr;
    map_app_close();
    audio_app_close();
    wifi_app_close();
    music_app_close();
    ai_voice_app_close();
}

/* =========================================================================
 * 5. ỨNG DỤNG SYSTEM MONITOR PRO MAX: DUAL ARC GAUGES + REALTIME LIVE CHART
 * ========================================================================= */
static void open_system_monitor_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "System Monitor Pro Max");
    lv_obj_clean(app_content_container);
    lv_obj_clear_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(app_content_container, 6, 0);

    // Khung trên: Dual Arc Speedometers (Đồng hồ tốc độ vòng tròn lớn 84x84)
    // ARC 1: CPU LOAD (Trái)
    arc_cpu = lv_arc_create(app_content_container);
    lv_obj_set_size(arc_cpu, 84, 84);
    lv_obj_set_pos(arc_cpu, 18, 6);
    lv_arc_set_rotation(arc_cpu, 135);
    lv_arc_set_bg_angles(arc_cpu, 0, 270);
    lv_arc_set_range(arc_cpu, 0, 100);
    lv_arc_set_value(arc_cpu, 35);
    lv_obj_set_style_arc_color(arc_cpu, lv_color_hex(0x00F2FE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_cpu, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_cpu, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_cpu, 7, LV_PART_MAIN);
    lv_obj_clear_flag(arc_cpu, LV_OBJ_FLAG_CLICKABLE);

    lbl_cpu_arc_val = lv_label_create(arc_cpu);
    lv_label_set_text(lbl_cpu_arc_val, "35%\nCPU");
    lv_obj_set_style_text_align(lbl_cpu_arc_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_cpu_arc_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_cpu_arc_val, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_cpu_arc_val);

    // ARC 2: RAM UTILIZATION (Phải)
    arc_ram = lv_arc_create(app_content_container);
    lv_obj_set_size(arc_ram, 84, 84);
    lv_obj_set_pos(arc_ram, 120, 6);
    lv_arc_set_rotation(arc_ram, 135);
    lv_arc_set_bg_angles(arc_ram, 0, 270);
    lv_arc_set_range(arc_ram, 0, 100);
    lv_arc_set_value(arc_ram, 28);
    lv_obj_set_style_arc_color(arc_ram, lv_color_hex(0x00E676), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_ram, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_ram, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_ram, 7, LV_PART_MAIN);
    lv_obj_clear_flag(arc_ram, LV_OBJ_FLAG_CLICKABLE);

    lbl_ram_arc_val = lv_label_create(arc_ram);
    lv_label_set_text(lbl_ram_arc_val, "28%\nRAM");
    lv_obj_set_style_text_align(lbl_ram_arc_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_ram_arc_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_ram_arc_val, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_ram_arc_val);

    // Khung Telemetry bên cạnh Arc (242x84)
    lv_obj_t *telemetry_box = lv_obj_create(app_content_container);
    lv_obj_set_size(telemetry_box, 246, 84);
    lv_obj_set_pos(telemetry_box, 222, 6);
    lv_obj_set_style_radius(telemetry_box, 10, 0);
    lv_obj_set_style_bg_color(telemetry_box, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(telemetry_box, lv_color_hex(0x2D3748), 0);
    lv_obj_set_style_border_width(telemetry_box, 1, 0);
    lv_obj_set_style_pad_all(telemetry_box, 6, 0);
    lv_obj_clear_flag(telemetry_box, LV_OBJ_FLAG_SCROLLABLE);

    lbl_temp_chip = lv_label_create(telemetry_box);
    lv_label_set_text(lbl_temp_chip, "🔥 Chip Temp: 41.8 °C");
    lv_obj_set_style_text_color(lbl_temp_chip, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_text_font(lbl_temp_chip, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_temp_chip, 4, 4);

    lbl_psram_chip = lv_label_create(telemetry_box);
    lv_label_set_text(lbl_psram_chip, "💾 PSRAM: 1.1 / 8.0 MB (Octal)");
    lv_obj_set_style_text_color(lbl_psram_chip, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_text_font(lbl_psram_chip, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_psram_chip, 4, 28);

    lbl_uptime_chip = lv_label_create(telemetry_box);
    lv_label_set_text(lbl_uptime_chip, "⏱ Uptime: 00:01:25 • Dual Core");
    lv_obj_set_style_text_color(lbl_uptime_chip, lv_color_hex(0xA0AEC0), 0);
    lv_obj_set_style_text_font(lbl_uptime_chip, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lbl_uptime_chip, 4, 52);

    // Khung dưới: REAL-TIME SCROLLING LINE CHART (Biểu đồ sóng thời gian thực 456x156)
    chart_system = lv_chart_create(app_content_container);
    lv_obj_set_size(chart_system, 456, 156);
    lv_obj_set_pos(chart_system, 12, 98);
    lv_chart_set_type(chart_system, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_system, 32); // 32 mẫu lịch sử cho màn hình 3.5"
    lv_chart_set_range(chart_system, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart_system, 4, 6);
    lv_obj_set_style_bg_color(chart_system, lv_color_hex(0x111622), 0);
    lv_obj_set_style_border_color(chart_system, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(chart_system, 1, 0);
    lv_obj_set_style_radius(chart_system, 8, 0);

    ser_cpu = lv_chart_add_series(chart_system, lv_color_hex(0x00F2FE), LV_CHART_AXIS_PRIMARY_Y);
    ser_ram = lv_chart_add_series(chart_system, lv_color_hex(0x00E676), LV_CHART_AXIS_PRIMARY_Y);

    // Khởi tạo các điểm ban đầu cho chart
    for (int i = 0; i < 32; i++)
    {
        lv_chart_set_next_value(chart_system, ser_cpu, 25 + (i % 15));
        lv_chart_set_next_value(chart_system, ser_ram, 28);
    }
}

/* =========================================================================
 * 6. ỨNG DỤNG GOOGLE MAPS PRO MAX
 * ========================================================================= */
static void open_map_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "Google Maps Pro Max");
    lv_obj_clean(app_content_container);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);

    map_app_open(app_content_container);
}

/* =========================================================================
 * 6B. ỨNG DỤNG VOICE AI & AUDIO LAB PRO MAX
 * ========================================================================= */
static void open_audio_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "Voice AI & Audio Lab Pro Max");
    lv_obj_clean(app_content_container);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);

    audio_app_open(app_content_container);
}

/* =========================================================================
 * 7. ỨNG DỤNG WIFI SETTINGS APP
 * ========================================================================= */
static void open_wifi_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "WiFi Settings App");
    lv_obj_clean(app_content_container);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);

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

/* =========================================================================
 * 8. ỨNG DỤNG SETTINGS CONTROL CENTER PRO MAX
 * ========================================================================= */
static void open_settings_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "Settings Control Center Pro Max");
    lv_obj_clean(app_content_container);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(app_content_container, 10, 0);

    // Card 1: Điều khiển độ sáng màn hình LCD PWM
    lv_obj_t *card_bright = lv_obj_create(app_content_container);
    lv_obj_set_size(card_bright, 460, 95);
    lv_obj_align(card_bright, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(card_bright, 12, 0);
    lv_obj_set_style_bg_color(card_bright, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(card_bright, lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_border_width(card_bright, 1, 0);
    lv_obj_clear_flag(card_bright, LV_OBJ_FLAG_SCROLLABLE);

    lbl_brightness_val = lv_label_create(card_bright);
    lv_label_set_text(lbl_brightness_val, "☀️ Độ sáng Màn hình IPS PWM: 85%");
    lv_obj_set_style_text_color(lbl_brightness_val, lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_text_font(lbl_brightness_val, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_brightness_val, LV_ALIGN_TOP_LEFT, 0, 0);

    slider_brightness = lv_slider_create(card_bright);
    lv_obj_set_size(slider_brightness, 420, 16);
    lv_obj_align(slider_brightness, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_slider_set_range(slider_brightness, 10, 100);
    lv_slider_set_value(slider_brightness, 85, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(0xFFB300), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(0xFFD54F), LV_PART_KNOB);
    lv_obj_add_event_cb(slider_brightness, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Card 2: Bộ chọn màu chủ đề Theme Accent Color
    lv_obj_t *card_theme = lv_obj_create(app_content_container);
    lv_obj_set_size(card_theme, 460, 115);
    lv_obj_align(card_theme, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(card_theme, 12, 0);
    lv_obj_set_style_bg_color(card_theme, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(card_theme, lv_color_hex(0x2D3748), 0);
    lv_obj_set_style_border_width(card_theme, 1, 0);
    lv_obj_clear_flag(card_theme, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *theme_title = lv_label_create(card_theme);
    lv_label_set_text(theme_title, "🎨 Màu Chủ Đề Accent Giao Diện:");
    lv_obj_set_style_text_color(theme_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(theme_title, &lv_font_montserrat_14, 0);
    lv_obj_align(theme_title, LV_ALIGN_TOP_LEFT, 0, 0);

    auto create_color_pill = [&](int x, uint32_t hex, const char *txt) {
        lv_obj_t *b = lv_btn_create(card_theme);
        lv_obj_set_size(b, 95, 34);
        lv_obj_set_pos(b, x, 32);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(hex), 0);
        lv_obj_add_event_cb(b, theme_color_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)hex);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(0x0A0D14), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_obj_center(l);
    };

    create_color_pill(10,  0x00F2FE, "Cyan Pro");
    create_color_pill(118, 0x00E676, "Emerald");
    create_color_pill(226, 0xFF3B30, "Neon Red");
    create_color_pill(334, 0x9D4EDD, "Purple OS");
}

/* =========================================================================
 * 9. ỨNG DỤNG SENSORS & TOOLS
 * ========================================================================= */
static void open_tools_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "Sensors & Telemetry Pro Max");
    lv_obj_clean(app_content_container);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(app_content_container, 10, 0);

    lv_obj_t *compass_card = lv_obj_create(app_content_container);
    lv_obj_set_size(compass_card, 460, 246);
    lv_obj_center(compass_card);
    lv_obj_set_style_radius(compass_card, 12, 0);
    lv_obj_set_style_bg_color(compass_card, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(compass_card, lv_color_hex(0x3A86FF), 0);
    lv_obj_set_style_border_width(compass_card, 1, 0);

    lv_obj_t *t = lv_label_create(compass_card);
    lv_label_set_text(t, "🧭 La Bàn Kỹ Thuật Số & Cảm Biến IMU 6-Axis");
    lv_obj_set_style_text_color(t, lv_color_hex(0x3A86FF), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    lbl_compass_val = lv_label_create(compass_card);
    lv_label_set_text(lbl_compass_val, "Hướng la bàn: 180.5° Nam (South)\nĐộ lệch từ trường: +1.2° | Cường độ: 48.2 µT");
    lv_obj_set_style_text_color(lbl_compass_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_compass_val, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_compass_val, LV_ALIGN_CENTER, 0, -18);

    lbl_pitch_val = lv_label_create(compass_card);
    lv_label_set_text(lbl_pitch_val, "Góc nghiêng (Pitch/Roll): 0.2° | -0.5°\nGia tốc kế: [X: 0.02, Y: -0.01, Z: 9.81 m/s²]\nÁp suất khí quyển: 1013.25 hPa • Độ cao ước tính: 12 m");
    lv_obj_set_style_text_color(lbl_pitch_val, lv_color_hex(0x718096), 0);
    lv_obj_set_style_text_font(lbl_pitch_val, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_pitch_val, LV_ALIGN_BOTTOM_MID, 0, -12);
}

/* =========================================================================
 * 10. ỨNG DỤNG ABOUT MINI OS PRO MAX
 * ========================================================================= */
static void open_about_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "About Mini OS Pro Max");
    lv_obj_clean(app_content_container);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(app_content_container, 10, 0);

    lv_obj_t *card = lv_obj_create(app_content_container);
    lv_obj_set_size(card, 460, 246);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x9D4EDD), 0);
    lv_obj_set_style_border_width(card, 1, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "✨ DIYMORE ESP32-S3 3.5\" IPS Mini OS Pro Max");
    lv_obj_set_style_text_color(title, lv_color_hex(0x9D4EDD), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *desc = lv_label_create(card);
    lv_label_set_text(desc, 
        "Hệ điều hành: Mini OS Pro Max v2.5 (Build 2026)\n"
        "Phần cứng: DIYMORE ESP32-S3 3.5\" IPS (XiaoZhi AI Native)\n"
        "Màn hình: 3.5 inch IPS 480x320 Pixels (Driver ST7796 SPI 40MHz DMA)\n"
        "Cảm ứng: Điện dung đa điểm I2C (Capacitive Touch FT6336U/GT911)\n"
        "Vi xử lý: ESP32-S3 Dual-Core LX7 @ 240MHz (Core 0 Net / Core 1 LVGL)\n"
        "Bộ nhớ: 16MB Flash + 8MB Octal OPI PSRAM (qio_opi)\n"
        "Bản đồ: Google Maps Dual-Engine + TJpgDec Hardware JPEG Downloader");
    lv_obj_set_style_text_color(desc, lv_color_hex(0xCBD5E0), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, 16);
}

/* =========================================================================
 * 11. ỨNG DỤNG MUSIC PLAYER PRO MAX (CHIA ĐÔI 2 CỘT & ĐĨA THAN QUAY)
 * ========================================================================= */
static void open_music_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "Music Player • /music SD");
    lv_obj_clean(app_content_container);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);
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

/* =========================================================================
 * 12. ỨNG DỤNG AI VOICE ASSISTANT (CHAT BUBBLES & PUSH-TO-TALK)
 * ========================================================================= */
static void open_ai_voice_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "XiaoZhi AI Voice • Gemini Assistant");
    lv_obj_clean(app_content_container);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);
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

/* =========================================================================
 * 13. ỨNG DỤNG CAMERA / RTSP STREAMER PRO MAX
 * ========================================================================= */
static void open_camera_app(void)
{
    ensure_app_window();
    lv_label_set_text(app_title_lbl, "Camera & RTSP Streamer");
    lv_obj_clean(app_content_container);
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_window, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(app_content_container, 10, 0);

    lv_obj_t *card = lv_obj_create(app_content_container);
    lv_obj_set_size(card, 460, 246);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xFF006E), 0);
    lv_obj_set_style_border_width(card, 1, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_IMAGE " Camera Sensor Subsystem");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF006E), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *desc = lv_label_create(card);
    bool cam_avail = camera_service_is_available();
    if (cam_avail)
    {
        lv_label_set_text_fmt(desc,
            "Trạng thái: Đã kết nối sensor [%s]\n"
            "Chế độ sẵn sàng: DVP 8-bit DMA / RTSP Video Server\n"
            "Độ phân giải hỗ trợ: QVGA (320x240) • HVGA (480x320) • VGA\n"
            "Bộ đệm Frame Buffer: Cấp phát trên 8MB Octal PSRAM\n"
            "Hỗ trợ chuẩn: ONVIF Profile S / RTSP H.264 / MJPEG",
            camera_service_get_model_name());
    }
    else
    {
        lv_label_set_text(desc,
            "⚠️ Camera Module Not Detected (Chưa cắm phần cứng)\n\n"
            "Hệ điều hành đã tích hợp sẵn Driver & Abstraction Layer:\n"
            "• Chuẩn kết nối hỗ trợ: DVP 8-bit song song (OV2640 / OV5640)\n"
            "• Lưu ý phần cứng: Chân DVP camera cần kiểm tra xung đột với\n"
            "  bus SPI LCD ST7796 và khe cắm thẻ nhớ MicroSD.\n"
            "• Sẵn sàng cho: Live Preview LVGL, RTSP Streamer & ONVIF NVTs.\n\n"
            "Trạng thái Subsystem: Idle / Ready for Hardware Initialization");
    }
    lv_obj_set_style_text_color(desc, lv_color_hex(0xCBD5E0), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, 16);
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
    if (lvgl_port_lock(1000))
    {
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0A0D14), 0);
        create_status_bar();
        create_desktop();
        lvgl_port_unlock();
    }
}

/* =========================================================================
 * CẬP NHẬT ĐỊNH KỲ THỜI GIAN THỰC (TỪ BACKGROUND LOOP)
 * ========================================================================= */
void ui_update_periodic(const SystemStats &stats)
{
    if (!lvgl_port_lock(200))
    {
        return; // Bỏ qua nhịp này nếu luồng giao diện đang bận vẽ frame
    }
    // 1. Cập nhật đồng hồ Status Bar
    if (lbl_clock)
    {
        uint32_t s = stats.uptime_sec;
        lv_label_set_text_fmt(lbl_clock, "%02u:%02u:%02u", s / 3600, (s % 3600) / 60, s % 60);
    }

    // 2. Cập nhật RAM Pill trên Status Bar
    if (lbl_ram_pill)
    {
        lv_label_set_text_fmt(lbl_ram_pill, "%d%%", stats.heap_usage_percent);
        if (stats.heap_usage_percent > 80)
            lv_obj_set_style_text_color(lbl_ram_pill, lv_color_hex(0xFF3B30), 0);
        else if (stats.heap_usage_percent > 50)
            lv_obj_set_style_text_color(lbl_ram_pill, lv_color_hex(0xFFB300), 0);
        else
            lv_obj_set_style_text_color(lbl_ram_pill, lv_color_hex(0x00E676), 0);
    }

    // 3. Cập nhật Icon WiFi trên Status Bar
    if (lbl_wifi_icon)
    {
        if (wifi_manager_is_connected())
        {
            lv_obj_set_style_text_color(lbl_wifi_icon, lv_color_hex(0x00E676), 0);
        }
        else
        {
            lv_obj_set_style_text_color(lbl_wifi_icon, lv_color_hex(0x718096), 0);
        }
    }

    // 4. Cập nhật System Monitor Pro Max (Dual Arc & Live Chart)
    if (arc_cpu && lbl_cpu_arc_val)
    {
        // CPU Load mô phỏng biến thiên thực tế 25-45%
        int simulated_cpu = 25 + (stats.uptime_sec % 20);
        lv_arc_set_value(arc_cpu, simulated_cpu);
        lv_label_set_text_fmt(lbl_cpu_arc_val, "%d%%\nCPU", simulated_cpu);

        if (chart_system && ser_cpu)
        {
            lv_chart_set_next_value(chart_system, ser_cpu, simulated_cpu);
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
        lv_label_set_text_fmt(lbl_temp_chip, "🔥 Temp: %.1f °C", stats.core_temp_c);
        if (stats.core_temp_c > 55.0f)
            lv_obj_set_style_text_color(lbl_temp_chip, lv_color_hex(0xFF3B30), 0);
        else
            lv_obj_set_style_text_color(lbl_temp_chip, lv_color_hex(0x00E676), 0);
    }

    if (lbl_psram_chip)
    {
        lv_label_set_text_fmt(lbl_psram_chip, "💾 PSRAM: %.1f/8 MB", (float)stats.used_psram / (1024.0f * 1024.0f));
    }

    if (lbl_uptime_chip)
    {
        lv_label_set_text_fmt(lbl_uptime_chip, "⏱ Up: %s", stats.uptime_str);
    }

    // 5. Cập nhật icon Loa trên Status Bar
    if (lbl_spk_icon)
    {
        if (audio_get_volume() > 0 && audio_is_pa_enabled())
        {
            lv_obj_set_style_text_color(lbl_spk_icon, lv_color_hex(0xFFB300), 0);
        }
        else
        {
            lv_obj_set_style_text_color(lbl_spk_icon, lv_color_hex(0x718096), 0);
        }
    }

    // 6. Cập nhật sóng âm thanh và VU Meter nếu ứng dụng Audio Lab đang mở
    audio_app_update();

    // 7. Cập nhật tiến trình quét mạng và trạng thái kết nối nếu ứng dụng WiFi App đang mở
    wifi_app_update();

    // 8. Cập nhật hiển thị bản đồ khi có ảnh JPEG mới từ thẻ SD hoặc mạng
    map_app_render();

    // 9. Cập nhật tiến trình phát nhạc và animation đĩa than Music Player
    music_app_update();

    // 10. Cập nhật trạng thái AI Voice Assistant và animation sóng âm
    ai_voice_app_update();

    lvgl_port_unlock();
}
