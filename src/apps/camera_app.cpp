/**
 * @file camera_app.cpp
 * @brief Giao diện ứng dụng Camera & RTSP Streamer: Tối ưu cho màn hình Portrait 240x320
 * Ưu tiên vùng ảnh Canvas lớn, toolbar phía dưới với touch target >= 32px, bàn phím ảo tích hợp
 */

#include "camera_app.h"
#include "../camera/camera_service.h"
#include "../display/lvgl_port.h"
#include "../ui/ui_theme.h"
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>

// Các widget giao diện
static lv_obj_t *main_container = nullptr;
static lv_obj_t *cam_canvas = nullptr;
static lv_color_t *cam_canvas_buf = nullptr;
static uint16_t canvas_w = 236;
static uint16_t canvas_h = 176;

static lv_obj_t *toolbar_box = nullptr;
static lv_obj_t *lbl_metrics = nullptr;
static lv_obj_t *lbl_cam_status = nullptr;
static lv_obj_t *btn_snap = nullptr;
static lv_obj_t *btn_cfg = nullptr;
static lv_obj_t *btn_disconnect = nullptr;

// Modal cấu hình & Bàn phím ảo
static lv_obj_t *cfg_modal = nullptr;
static lv_obj_t *cam_keyboard = nullptr;
static lv_obj_t *ta_name = nullptr;
static lv_obj_t *ta_ip = nullptr;
static lv_obj_t *ta_http_port = nullptr;
static lv_obj_t *ta_rtsp_port = nullptr;
static lv_obj_t *ta_onvif_port = nullptr;
static lv_obj_t *ta_user = nullptr;
static lv_obj_t *ta_pass = nullptr;
static lv_obj_t *dd_vendor = nullptr;
static lv_obj_t *dd_proto = nullptr;
static lv_obj_t *btn_save_connect = nullptr;

// Thống kê thực tế (True Telemetry)
static uint32_t last_rendered_frame_id = 0;
static uint32_t last_frame_time_ms = 0;
static uint32_t frame_count = 0;
static float real_fps = 0.0f;
static uint32_t last_fps_calc_time = 0;

static int16_t draw_offset_x = 0;
static int16_t draw_offset_y = 0;

/* Callback của thư viện TJpgDec đưa dữ liệu RGB565 vào bộ đệm Canvas */
static bool camera_tjpg_output_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    if (!cam_canvas_buf) return false;
    for (int16_t row = 0; row < h; row++)
    {
        int16_t dy = y + row + draw_offset_y;
        if (dy < 0) continue;
        if (dy >= canvas_h) break;
        for (int16_t col = 0; col < w; col++)
        {
            int16_t dx = x + col + draw_offset_x;
            if (dx < 0) continue;
            if (dx >= canvas_w) break;
            cam_canvas_buf[dy * canvas_w + dx] = lv_color_make(
                ((bitmap[row * w + col] >> 11) & 0x1F) << 3,
                ((bitmap[row * w + col] >> 5) & 0x3F) << 2,
                (bitmap[row * w + col] & 0x1F) << 3
            );
        }
    }
    return true;
}

/* Đọc thông số cấu hình từ giao diện UI và áp dụng vào Camera Service */
static void apply_ui_configuration(bool start_after_config)
{
    NetworkCameraProfile prof;
    memset(&prof, 0, sizeof(prof));

    if (ta_name) strncpy(prof.name, lv_textarea_get_text(ta_name), sizeof(prof.name) - 1);
    if (ta_ip) strncpy(prof.ip, lv_textarea_get_text(ta_ip), sizeof(prof.ip) - 1);
    if (ta_http_port) prof.http_port = (uint16_t)atoi(lv_textarea_get_text(ta_http_port));
    if (ta_rtsp_port) prof.rtsp_port = (uint16_t)atoi(lv_textarea_get_text(ta_rtsp_port));
    if (ta_onvif_port) prof.onvif_port = (uint16_t)atoi(lv_textarea_get_text(ta_onvif_port));
    if (ta_user) strncpy(prof.username, lv_textarea_get_text(ta_user), sizeof(prof.username) - 1);
    if (ta_pass) strncpy(prof.password, lv_textarea_get_text(ta_pass), sizeof(prof.password) - 1);

    if (dd_vendor)
    {
        uint16_t v_idx = lv_dropdown_get_selected(dd_vendor);
        switch (v_idx)
        {
            case 1: prof.vendor = CAM_VENDOR_HIKVISION; break;
            case 2: prof.vendor = CAM_VENDOR_KBVISION; break;
            case 3: prof.vendor = CAM_VENDOR_EZVIZ; break;
            case 4: prof.vendor = CAM_VENDOR_YOOSEE; break;
            case 0:
            default: prof.vendor = CAM_VENDOR_GENERIC_ONVIF; break;
        }
    }

    if (dd_proto)
    {
        uint16_t p_idx = lv_dropdown_get_selected(dd_proto);
        switch (p_idx)
        {
            case 1: prof.protocol = CAM_PROTO_MJPEG; break;
            case 2: prof.protocol = CAM_PROTO_RTSP; break;
            case 0:
            default: prof.protocol = CAM_PROTO_HTTP_SNAPSHOT; break;
        }
    }

    camera_service_configure_network(prof);
    camera_service_save_network_profile();

    if (start_after_config)
    {
        camera_service_start();
    }
}

// Bấm nút Refresh Snapshot
static void btn_snap_cb(lv_event_t *e)
{
    camera_service_start();
}

// Bấm nút Mở / Đóng Cấu hình
static void btn_cfg_cb(lv_event_t *e)
{
    if (!cfg_modal) return;
    if (lv_obj_has_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_clear_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(cfg_modal);
    }
    else
    {
        if (cam_keyboard) lv_obj_add_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

// Bấm nút Ngắt kết nối
static void btn_disconnect_cb(lv_event_t *e)
{
    camera_service_stop();
    if (lbl_cam_status)
    {
        lv_label_set_text(lbl_cam_status, "Đã ngắt kết nối");
        lv_obj_set_style_text_color(lbl_cam_status, lv_color_hex(COLOR_TEXT_MUTED), 0);
    }
}

// Bấm Lưu & Kết nối trong Modal Cấu hình
static void btn_save_connect_cb(lv_event_t *e)
{
    if (cam_keyboard) lv_obj_add_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);
    apply_ui_configuration(true);
    if (cfg_modal)
    {
        lv_obj_add_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

// Callback khi focus vào ô nhập liệu
static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (cam_keyboard && ta)
    {
        lv_keyboard_set_textarea(cam_keyboard, ta);
        lv_obj_clear_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(cam_keyboard);
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    }
}

// Callback phím ảo bàn phím
static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    {
        if (cam_keyboard) lv_obj_add_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

/* =========================================================================
 * KHỞI TẠO GIAO DIỆN CAMERA 240x320 PORTRAIT
 * ========================================================================= */
void camera_app_open(lv_obj_t *parent)
{
    main_container = parent;
    lv_obj_set_style_pad_all(parent, 2, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    last_rendered_frame_id = 0;
    frame_count = 0;
    real_fps = 0.0f;
    last_fps_calc_time = millis();
    last_frame_time_ms = millis();

    // 1. CẤP PHÁT BỘ ĐỆM CANVAS (236 x 176 RGB565 = ~83 KB trong PSRAM)
    if (!cam_canvas_buf)
    {
        cam_canvas_buf = (lv_color_t *)heap_caps_malloc(canvas_w * canvas_h * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        if (!cam_canvas_buf)
        {
            cam_canvas_buf = (lv_color_t *)malloc(canvas_w * canvas_h * sizeof(lv_color_t));
        }
    }

    if (cam_canvas_buf)
    {
        // Khởi tạo màu đen cho canvas
        for (int i = 0; i < canvas_w * canvas_h; i++)
        {
            cam_canvas_buf[i] = lv_color_hex(0x0A0D14);
        }
    }

    // 2. VÙNG KHUNG HÌNH CAMERA ƯU TIÊN LỚN (236x176)
    cam_canvas = lv_canvas_create(parent);
    if (cam_canvas_buf)
    {
        lv_canvas_set_buffer(cam_canvas, cam_canvas_buf, canvas_w, canvas_h, LV_IMG_CF_TRUE_COLOR);
    }
    lv_obj_set_size(cam_canvas, canvas_w, canvas_h);
    lv_obj_align(cam_canvas, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_border_color(cam_canvas, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(cam_canvas, 1, 0);
    lv_obj_set_style_radius(cam_canvas, 8, 0);

    // 3. TOOLBAR NHỎ PHÍA DƯỚI (KHÔNG CHE VÙNG ẢNH)
    toolbar_box = lv_obj_create(parent);
    lv_obj_set_size(toolbar_box, SCREEN_WIDTH - 6, APP_CONTENT_HEIGHT - canvas_h - 10);
    lv_obj_align(toolbar_box, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(toolbar_box, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(toolbar_box, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(toolbar_box, 1, 0);
    lv_obj_set_style_radius(toolbar_box, 10, 0);
    lv_obj_set_style_pad_all(toolbar_box, 4, 0);
    lv_obj_clear_flag(toolbar_box, LV_OBJ_FLAG_SCROLLABLE);

    // Hàng nút điều khiển nhỏ: Chụp lại / Cấu hình / Ngắt (Touch Target >= 32px)
    lv_obj_t *btn_row = lv_obj_create(toolbar_box);
    lv_obj_set_size(btn_row, SCREEN_WIDTH - 16, 34);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    // Nút Snapshot
    btn_snap = lv_btn_create(btn_row);
    lv_obj_set_size(btn_snap, 72, 32);
    lv_obj_set_ext_click_area(btn_snap, 4);
    lv_obj_align(btn_snap, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(btn_snap, 6, 0);
    lv_obj_set_style_bg_color(btn_snap, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_add_event_cb(btn_snap, btn_snap_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_snap = lv_label_create(btn_snap);
    lv_label_set_text(lbl_snap, LV_SYMBOL_REFRESH " Chụp");
    lv_obj_set_style_text_color(lbl_snap, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_text_font(lbl_snap, UI_FONT_10, 0);
    lv_obj_center(lbl_snap);

    // Nút Cấu hình
    btn_cfg = lv_btn_create(btn_row);
    lv_obj_set_size(btn_cfg, 76, 32);
    lv_obj_set_ext_click_area(btn_cfg, 4);
    lv_obj_align(btn_cfg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(btn_cfg, 6, 0);
    lv_obj_set_style_bg_color(btn_cfg, lv_color_hex(0x1F2A3D), 0);
    lv_obj_set_style_border_color(btn_cfg, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_width(btn_cfg, 1, 0);
    lv_obj_add_event_cb(btn_cfg, btn_cfg_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_cfg = lv_label_create(btn_cfg);
    lv_label_set_text(lbl_cfg, LV_SYMBOL_SETTINGS " Cài đặt");
    lv_obj_set_style_text_color(lbl_cfg, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_cfg, UI_FONT_10, 0);
    lv_obj_center(lbl_cfg);

    // Nút Ngắt kết nối
    btn_disconnect = lv_btn_create(btn_row);
    lv_obj_set_size(btn_disconnect, 68, 32);
    lv_obj_set_ext_click_area(btn_disconnect, 4);
    lv_obj_align(btn_disconnect, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(btn_disconnect, 6, 0);
    lv_obj_set_style_bg_color(btn_disconnect, lv_color_hex(0x281B24), 0);
    lv_obj_set_style_border_color(btn_disconnect, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_border_width(btn_disconnect, 1, 0);
    lv_obj_add_event_cb(btn_disconnect, btn_disconnect_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_dis = lv_label_create(btn_disconnect);
    lv_label_set_text(lbl_dis, LV_SYMBOL_POWER " Dừng");
    lv_obj_set_style_text_color(lbl_dis, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_text_font(lbl_dis, UI_FONT_10, 0);
    lv_obj_center(lbl_dis);

    // Dòng thông số thực tế (FPS, Độ phân giải, Độ trễ)
    lbl_metrics = lv_label_create(toolbar_box);
    lv_label_set_text(lbl_metrics, "FPS: 0.0 • 0x0 • 0 KB • 0ms");
    lv_obj_set_style_text_color(lbl_metrics, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_text_font(lbl_metrics, UI_FONT_10, 0);
    lv_obj_align(lbl_metrics, LV_ALIGN_BOTTOM_LEFT, 2, -18);

    // Dòng trạng thái nguồn & Năng lực thực
    lbl_cam_status = lv_label_create(toolbar_box);
    lv_label_set_text(lbl_cam_status, "HTTP Snap: Sẵn sàng | ONVIF/RTSP: Chưa hỗ trợ");
    lv_obj_set_style_text_color(lbl_cam_status, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_cam_status, UI_FONT_10, 0);
    lv_obj_align(lbl_cam_status, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    // 4. MODAL CẤU HÌNH CAMERA CUỘN DỌC (MẶC ĐỊNH ẨN)
    cfg_modal = lv_obj_create(parent);
    lv_obj_set_size(cfg_modal, SCREEN_WIDTH - 4, APP_CONTENT_HEIGHT);
    lv_obj_center(cfg_modal);
    lv_obj_set_style_radius(cfg_modal, 12, 0);
    lv_obj_set_style_bg_color(cfg_modal, lv_color_hex(0x0C101A), 0);
    lv_obj_set_style_border_color(cfg_modal, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_width(cfg_modal, 1, 0);
    lv_obj_set_style_pad_all(cfg_modal, 6, 0);
    lv_obj_add_flag(cfg_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN); // Ẩn ban đầu

    // Header modal cấu hình
    lv_obj_t *m_hdr = lv_label_create(cfg_modal);
    lv_label_set_text(m_hdr, LV_SYMBOL_SETTINGS " Cấu Hình IP Cam");
    lv_obj_set_style_text_color(m_hdr, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_text_font(m_hdr, UI_FONT_12, 0);
    lv_obj_align(m_hdr, LV_ALIGN_TOP_LEFT, 4, 4);

    // Nút Đóng modal (Touch target >= 32x32)
    lv_obj_t *btn_m_close = lv_btn_create(cfg_modal);
    lv_obj_set_size(btn_m_close, 32, 32);
    lv_obj_set_ext_click_area(btn_m_close, 4);
    lv_obj_align(btn_m_close, LV_ALIGN_TOP_RIGHT, -2, 0);
    lv_obj_set_style_radius(btn_m_close, 6, 0);
    lv_obj_set_style_bg_color(btn_m_close, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_add_event_cb(btn_m_close, btn_cfg_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_mc = lv_label_create(btn_m_close);
    lv_label_set_text(lbl_mc, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lbl_mc, UI_FONT_12, 0);
    lv_obj_center(lbl_mc);

    const NetworkCameraProfile &cur_prof = camera_service_get_network_profile();

    // Helper tạo ô nhập liệu
    auto make_input = [&](const char *label_text, const char *default_val, lv_coord_t y_pos, bool is_pwd = false) -> lv_obj_t* {
        lv_obj_t *lbl = lv_label_create(cfg_modal);
        lv_label_set_text(lbl, label_text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(lbl, UI_FONT_10, 0);
        lv_obj_set_pos(lbl, 6, y_pos);

        lv_obj_t *ta = lv_textarea_create(cfg_modal);
        lv_obj_set_size(ta, SCREEN_WIDTH - 28, 30);
        lv_obj_set_pos(ta, 6, y_pos + 14);
        lv_textarea_set_text(ta, default_val);
        lv_textarea_set_one_line(ta, true);
        if (is_pwd) lv_textarea_set_password_mode(ta, true);
        lv_obj_set_style_bg_color(ta, lv_color_hex(0x151B27), 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(0x2A354A), 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_set_style_text_font(ta, UI_FONT_10, 0);
        lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
        return ta;
    };

    char buf_port[16];
    snprintf(buf_port, sizeof(buf_port), "%u", cur_prof.http_port > 0 ? cur_prof.http_port : 80);
    char buf_rtsp[16];
    snprintf(buf_rtsp, sizeof(buf_rtsp), "%u", cur_prof.rtsp_port > 0 ? cur_prof.rtsp_port : 554);
    char buf_onvif[16];
    snprintf(buf_onvif, sizeof(buf_onvif), "%u", cur_prof.onvif_port > 0 ? cur_prof.onvif_port : 8000);

    ta_name = make_input("Tên Camera:", cur_prof.name[0] ? cur_prof.name : "IP Camera", 36);
    ta_ip = make_input("Địa chỉ IP:", cur_prof.ip[0] ? cur_prof.ip : "192.168.1.50", 84);
    ta_http_port = make_input("HTTP Snapshot Port:", buf_port, 132);
    ta_rtsp_port = make_input("RTSP Port:", buf_rtsp, 180);
    ta_onvif_port = make_input("ONVIF Port:", buf_onvif, 228);
    ta_user = make_input("Tài khoản (Username):", cur_prof.username[0] ? cur_prof.username : "admin", 276);
    ta_pass = make_input("Mật khẩu (Password):", cur_prof.password, 324, true);

    // Dropdown Hãng
    lv_obj_t *lbl_v = lv_label_create(cfg_modal);
    lv_label_set_text(lbl_v, "Nhà sản xuất (Vendor):");
    lv_obj_set_style_text_color(lbl_v, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_v, UI_FONT_10, 0);
    lv_obj_set_pos(lbl_v, 6, 372);

    dd_vendor = lv_dropdown_create(cfg_modal);
    lv_obj_set_size(dd_vendor, SCREEN_WIDTH - 28, 30);
    lv_obj_set_pos(dd_vendor, 6, 388);
    lv_dropdown_set_options(dd_vendor, "Generic ONVIF\nHikvision\nKBVision\nEZVIZ\nYoosee");
    lv_dropdown_set_selected(dd_vendor, (uint16_t)cur_prof.vendor);
    lv_obj_set_style_bg_color(dd_vendor, lv_color_hex(0x151B27), 0);
    lv_obj_set_style_text_font(dd_vendor, UI_FONT_10, 0);

    // Dropdown Giao thức
    lv_obj_t *lbl_p = lv_label_create(cfg_modal);
    lv_label_set_text(lbl_p, "Giao thức (Protocol):");
    lv_obj_set_style_text_color(lbl_p, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_p, UI_FONT_10, 0);
    lv_obj_set_pos(lbl_p, 6, 424);

    dd_proto = lv_dropdown_create(cfg_modal);
    lv_obj_set_size(dd_proto, SCREEN_WIDTH - 28, 30);
    lv_obj_set_pos(dd_proto, 6, 440);
    lv_dropdown_set_options(dd_proto, "HTTP Snapshot (OK)\nMJPEG (Chưa)\nRTSP/H.264 (Chưa)");
    lv_dropdown_set_selected(dd_proto, (uint16_t)cur_prof.protocol);
    lv_obj_set_style_bg_color(dd_proto, lv_color_hex(0x151B27), 0);
    lv_obj_set_style_text_font(dd_proto, UI_FONT_10, 0);

    // Nút Lưu & Kết nối (Touch target >= 32px)
    btn_save_connect = lv_btn_create(cfg_modal);
    lv_obj_set_size(btn_save_connect, SCREEN_WIDTH - 28, 34);
    lv_obj_set_ext_click_area(btn_save_connect, 4);
    lv_obj_set_pos(btn_save_connect, 6, 480);
    lv_obj_set_style_radius(btn_save_connect, 6, 0);
    lv_obj_set_style_bg_color(btn_save_connect, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_add_event_cb(btn_save_connect, btn_save_connect_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_save = lv_label_create(btn_save_connect);
    lv_label_set_text(lbl_save, LV_SYMBOL_SAVE " Lưu & Kết Nối");
    lv_obj_set_style_text_color(lbl_save, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_text_font(lbl_save, UI_FONT_12, 0);
    lv_obj_center(lbl_save);

    // 5. BÀN PHÍM ẢO TOÀN CHIỀU RỘNG (MẶC ĐỊNH ẨN)
    cam_keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(cam_keyboard, SCREEN_WIDTH - 4, 128);
    lv_obj_align(cam_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(cam_keyboard, kb_event_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);

    // Khởi tạo decoder TJpg
    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(camera_tjpg_output_cb);

    // Bắt đầu chạy service camera
    camera_service_start();
}

/* Đóng và dọn dẹp */
void camera_app_close(void)
{
    camera_service_stop();
    main_container = nullptr;
    cam_canvas = nullptr;
    toolbar_box = nullptr;
    lbl_metrics = nullptr;
    lbl_cam_status = nullptr;
    btn_snap = nullptr;
    btn_cfg = nullptr;
    btn_disconnect = nullptr;
    cfg_modal = nullptr;
    cam_keyboard = nullptr;
}

/* Cập nhật định kỳ */
void camera_app_update(void)
{
    if (!main_container || !cam_canvas || !cam_canvas_buf) return;

    // Lấy frame mới nhất từ Ping-Pong Double Buffer
    CameraFrame *frame = camera_service_get_frame();
    if (frame && frame->buf && frame->len > 0)
    {
        if (frame->frame_id != last_rendered_frame_id)
        {
            last_rendered_frame_id = frame->frame_id;
            uint32_t now = millis();
            frame_count++;
            if (now - last_fps_calc_time >= 1000)
            {
                real_fps = (float)frame_count * 1000.0f / (float)(now - last_fps_calc_time);
                frame_count = 0;
                last_fps_calc_time = now;
            }

            uint32_t latency = (frame->timestamp_ms > 0 && now >= frame->timestamp_ms)
                               ? (now - frame->timestamp_ms) : (now - last_frame_time_ms);
            last_frame_time_ms = now;

            // Đọc kích thước ảnh JPEG gốc và tính tỷ lệ thu nhỏ
            uint16_t orig_w = 0, orig_h = 0;
            uint8_t scale = 1;
            if (TJpgDec.getJpgSize(&orig_w, &orig_h, frame->buf, frame->len) == 0 && orig_w > 0 && orig_h > 0)
            {
                if (orig_w >= canvas_w * 8 || orig_h >= canvas_h * 8) scale = 8;
                else if (orig_w >= canvas_w * 4 || orig_h >= canvas_h * 4) scale = 4;
                else if (orig_w >= canvas_w * 2 || orig_h >= canvas_h * 2) scale = 2;
                else scale = 1;

                TJpgDec.setJpgScale(scale);
                uint16_t scaled_w = orig_w / scale;
                uint16_t scaled_h = orig_h / scale;
                draw_offset_x = (canvas_w > scaled_w) ? (canvas_w - scaled_w) / 2 : 0;
                draw_offset_y = (canvas_h > scaled_h) ? (canvas_h - scaled_h) / 2 : 0;
            }
            else
            {
                TJpgDec.setJpgScale(1);
                draw_offset_x = 0;
                draw_offset_y = 0;
            }

            // Xóa nền đen để chống lem khi letterbox
            for (int i = 0; i < canvas_w * canvas_h; i++)
            {
                cam_canvas_buf[i] = lv_color_hex(0x000000);
            }

            // Giải mã JPEG an toàn vào Canvas
            TJpgDec.drawJpg(0, 0, frame->buf, frame->len);
            lv_obj_invalidate(cam_canvas);

            if (lbl_metrics)
            {
                lv_label_set_text_fmt(lbl_metrics, "FPS: %.1f • %dx%d • %uKB • %ums",
                    real_fps,
                    orig_w > 0 ? orig_w : (frame->width > 0 ? frame->width : canvas_w),
                    orig_h > 0 ? orig_h : (frame->height > 0 ? frame->height : canvas_h),
                    (unsigned int)(frame->len / 1024),
                    (unsigned int)latency);
            }
        }

        camera_service_return_frame(frame);
    }

    if (lbl_cam_status)
    {
        lv_label_set_text_fmt(lbl_cam_status, "%s (Snapshot: %s)",
            camera_service_get_status_text(),
            camera_feature_status_to_string(camera_service_get_snapshot_status()));
    }
}
