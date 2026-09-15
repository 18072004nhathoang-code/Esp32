/**
 * @file wifi_app.cpp
 * @brief Triển khai module WiFi Settings App trên LVGL 8 cho màn hình Portrait 240x320
 * Bố cục: Danh sách mạng full-width, chọn WiFi mở modal nhập mật khẩu, không chia đôi màn hình.
 */

#include "wifi_app.h"
#include "../os/wifi_manager.h"
#include "../ui/ui_theme.h"
#include <stdio.h>
#include <string.h>

// Quản lý trạng thái giao diện
static lv_obj_t *main_container = nullptr;
static lv_obj_t *top_bar = nullptr;
static lv_obj_t *btn_scan = nullptr;
static lv_obj_t *lbl_scan_info = nullptr;
static lv_obj_t *network_list = nullptr;

// Thành phần Modal nhập mật khẩu
static lv_obj_t *pwd_modal = nullptr;
static lv_obj_t *lbl_target_ssid = nullptr;
static lv_obj_t *ta_password = nullptr;
static lv_obj_t *btn_eye = nullptr;
static lv_obj_t *lbl_eye = nullptr;
static lv_obj_t *btn_connect = nullptr;
static lv_obj_t *btn_cancel = nullptr;
static lv_obj_t *btn_forget = nullptr;
static lv_obj_t *lbl_status = nullptr;
static lv_obj_t *keyboard = nullptr;

// Bộ nhớ đệm dữ liệu
static char current_selected_ssid[33] = {0};
static bool is_pwd_visible = false;
static bool is_active = false;
static bool need_list_refresh = false;
static std::vector<WiFiNetworkInfo> cached_scan_results;

// Hàm chuẩn hóa và bảo vệ UTF-8 cho SSID
static void sanitize_ssid(const char *raw_ssid, char *safe_ssid, size_t max_len)
{
    if (!raw_ssid || !safe_ssid || max_len == 0) return;

    size_t in_idx = 0;
    size_t out_idx = 0;
    size_t raw_len = strlen(raw_ssid);

    while (in_idx < raw_len && out_idx + 4 < max_len)
    {
        uint8_t c = (uint8_t)raw_ssid[in_idx];

        if (c < 0x20 || c == 0x7F)
        {
            in_idx++;
            continue;
        }

        if (c < 0x80)
        {
            safe_ssid[out_idx++] = (char)c;
            in_idx++;
            continue;
        }

        if ((c & 0xE0) == 0xC0)
        {
            if (in_idx + 1 < raw_len && ((uint8_t)raw_ssid[in_idx + 1] & 0xC0) == 0x80)
            {
                safe_ssid[out_idx++] = (char)c;
                safe_ssid[out_idx++] = raw_ssid[in_idx + 1];
                in_idx += 2;
                continue;
            }
            safe_ssid[out_idx++] = '?';
            in_idx++;
            continue;
        }

        if ((c & 0xF0) == 0xE0)
        {
            if (in_idx + 2 < raw_len && 
                ((uint8_t)raw_ssid[in_idx + 1] & 0xC0) == 0x80 && 
                ((uint8_t)raw_ssid[in_idx + 2] & 0xC0) == 0x80)
            {
                safe_ssid[out_idx++] = (char)c;
                safe_ssid[out_idx++] = raw_ssid[in_idx + 1];
                safe_ssid[out_idx++] = raw_ssid[in_idx + 2];
                in_idx += 3;
                continue;
            }
            safe_ssid[out_idx++] = '?';
            in_idx++;
            continue;
        }

        if ((c & 0xF8) == 0xF0)
        {
            if (in_idx + 3 < raw_len) in_idx += 4;
            else in_idx++;
            safe_ssid[out_idx++] = '?';
            continue;
        }

        safe_ssid[out_idx++] = '?';
        in_idx++;
    }

    safe_ssid[out_idx] = '\0';
}


/* =========================================================================
 * CÁC HÀM XỬ LÝ SỰ KIỆN (EVENT CALLBACKS)
 * ========================================================================= */

// Bấm nút Quét Mạng
static void scan_btn_event_cb(lv_event_t *e)
{
    if (lbl_scan_info)
    {
        lv_label_set_text(lbl_scan_info, "Đang quét...");
        lv_obj_set_style_text_color(lbl_scan_info, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    }
    wifi_manager_scan_async();
}

// Đóng modal nhập mật khẩu
static void close_modal_cb(lv_event_t *e)
{
    if (pwd_modal)
    {
        lv_obj_add_flag(pwd_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

// Bấm nút Kết Nối từ modal
static void connect_btn_event_cb(lv_event_t *e)
{
    if (strlen(current_selected_ssid) == 0) return;

    const char *pwd = "";
    if (ta_password)
    {
        pwd = lv_textarea_get_text(ta_password);
    }

    if (lbl_status)
    {
        char s_name[48]; sanitize_ssid(current_selected_ssid, s_name, sizeof(s_name));
        lv_label_set_text_fmt(lbl_status, LV_SYMBOL_REFRESH " Đang kết nối tới %s...", s_name);
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(COLOR_ACCENT_AMBER), 0);
    }

    wifi_manager_connect(current_selected_ssid, pwd);
}

// Bấm nút Quên Mạng (Xóa NVS)
static void forget_btn_event_cb(lv_event_t *e)
{
    wifi_manager_forget_network();
    if (lbl_status)
    {
        lv_label_set_text(lbl_status, LV_SYMBOL_TRASH " Đã quên mạng khỏi hệ thống");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(COLOR_ACCENT_RED), 0);
    }
    if (ta_password)
    {
        lv_textarea_set_text(ta_password, "");
    }
}

// Bấm nút hiện/ẩn mật khẩu (Eye button)
static void eye_btn_event_cb(lv_event_t *e)
{
    if (!ta_password || !lbl_eye) return;
    is_pwd_visible = !is_pwd_visible;
    lv_textarea_set_password_mode(ta_password, !is_pwd_visible);
    lv_label_set_text(lbl_eye, is_pwd_visible ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

// Sự kiện bàn phím ảo (nhấn OK / Apply trên keyboard)
static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY)
    {
        connect_btn_event_cb(nullptr);
    }
    else if (code == LV_EVENT_CANCEL)
    {
        close_modal_cb(nullptr);
    }
}

// Bấm chọn một mạng trong danh sách -> MỞ MODAL NHẬP MẬT KHẨU
static void network_item_clicked_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid || strlen(ssid) == 0) return;

    strncpy(current_selected_ssid, ssid, sizeof(current_selected_ssid) - 1);
    current_selected_ssid[sizeof(current_selected_ssid) - 1] = '\0';

    if (lbl_target_ssid)
    {
        char s_name[48]; sanitize_ssid(current_selected_ssid, s_name, sizeof(s_name));
    lv_label_set_text_fmt(lbl_target_ssid, "Mạng: %s", s_name);
    }

    if (ta_password)
    {
        lv_textarea_set_text(ta_password, "");
        lv_obj_clear_state(ta_password, LV_STATE_FOCUSED);
        lv_obj_add_state(ta_password, LV_STATE_FOCUSED);
    }

    if (lbl_status)
    {
        lv_label_set_text(lbl_status, "Nhập mật khẩu để kết nối");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    }

    if (pwd_modal)
    {
        lv_obj_clear_flag(pwd_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(pwd_modal);
    }
}

/* =========================================================================
 * XÂY DỰNG GIAO DIỆN FULL-WIDTH 240x320
 * ========================================================================= */
void wifi_app_open(lv_obj_t *parent)
{
    main_container = parent;
    is_active = true;

    lv_obj_set_style_pad_all(parent, 4, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // 1. THANH CÔNG CỤ TRÊN CÙNG (Y = 0, H = 34)
    top_bar = lv_obj_create(main_container);
    lv_obj_set_size(top_bar, SCREEN_WIDTH - 8, 34);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(top_bar, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(top_bar, 1, 0);
    lv_obj_set_style_radius(top_bar, 8, 0);
    lv_obj_set_style_pad_hor(top_bar, 6, 0);
    lv_obj_set_style_pad_ver(top_bar, 2, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Nút Quét Mạng
    btn_scan = lv_btn_create(top_bar);
    lv_obj_set_size(btn_scan, 84, 28);
    lv_obj_set_ext_click_area(btn_scan, 4);
    lv_obj_align(btn_scan, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(btn_scan, 6, 0);
    lv_obj_set_style_bg_color(btn_scan, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_add_event_cb(btn_scan, scan_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_scan_btn = lv_label_create(btn_scan);
    lv_label_set_text(lbl_scan_btn, LV_SYMBOL_REFRESH " Quét");
    lv_obj_set_style_text_color(lbl_scan_btn, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_text_font(lbl_scan_btn, UI_FONT_12, 0);
    lv_obj_center(lbl_scan_btn);

    // Nhãn trạng thái quét
    lbl_scan_info = lv_label_create(top_bar);
    lv_label_set_text(lbl_scan_info, "Sẵn sàng");
    lv_obj_set_style_text_color(lbl_scan_info, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_scan_info, UI_FONT_10, 0);
    lv_obj_align(lbl_scan_info, LV_ALIGN_RIGHT_MID, -4, 0);

    // 2. DANH SÁCH MẠNG FULL-WIDTH DẠNG LIST CUỘN DỌC
    network_list = lv_list_create(main_container);
    lv_obj_set_size(network_list, SCREEN_WIDTH - 8, APP_CONTENT_HEIGHT - 44);
    lv_obj_align(network_list, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(network_list, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(network_list, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(network_list, 1, 0);
    lv_obj_set_style_radius(network_list, 10, 0);
    lv_obj_set_style_pad_all(network_list, 4, 0);

    // 3. MODAL NHẬP MẬT KHẨU TOÀN CHIỀU RỘNG (MẶC ĐỊNH ẨN)
    pwd_modal = lv_obj_create(main_container);
    lv_obj_set_size(pwd_modal, SCREEN_WIDTH - 4, APP_CONTENT_HEIGHT);
    lv_obj_center(pwd_modal);
    lv_obj_set_style_radius(pwd_modal, 12, 0);
    lv_obj_set_style_bg_color(pwd_modal, lv_color_hex(0x0B1018), 0);
    lv_obj_set_style_border_color(pwd_modal, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_border_width(pwd_modal, 1, 0);
    lv_obj_set_style_pad_all(pwd_modal, 6, 0);
    lv_obj_clear_flag(pwd_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pwd_modal, LV_OBJ_FLAG_HIDDEN); // Ẩn ban đầu

    // Header modal
    lv_obj_t *m_header = lv_obj_create(pwd_modal);
    lv_obj_set_size(m_header, SCREEN_WIDTH - 16, 26);
    lv_obj_align(m_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(m_header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m_header, 0, 0);
    lv_obj_set_style_pad_all(m_header, 0, 0);
    lv_obj_clear_flag(m_header, LV_OBJ_FLAG_SCROLLABLE);

    lbl_target_ssid = lv_label_create(m_header);
    lv_label_set_text(lbl_target_ssid, "Mạng: --");
    lv_obj_set_style_text_color(lbl_target_ssid, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_text_font(lbl_target_ssid, UI_FONT_12, 0);
    lv_obj_align(lbl_target_ssid, LV_ALIGN_LEFT_MID, 0, 0);

    btn_cancel = lv_btn_create(m_header);
    lv_obj_set_size(btn_cancel, 32, 24);
    lv_obj_set_ext_click_area(btn_cancel, 6);
    lv_obj_align(btn_cancel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(btn_cancel, 6, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_add_event_cb(btn_cancel, close_modal_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_cx = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cx, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lbl_cx, UI_FONT_10, 0);
    lv_obj_center(lbl_cx);

    // Hàng nhập mật khẩu
    lv_obj_t *pwd_row = lv_obj_create(pwd_modal);
    lv_obj_set_size(pwd_row, SCREEN_WIDTH - 16, 34);
    lv_obj_align(pwd_row, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_opa(pwd_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pwd_row, 0, 0);
    lv_obj_set_style_pad_all(pwd_row, 0, 0);
    lv_obj_clear_flag(pwd_row, LV_OBJ_FLAG_SCROLLABLE);

    ta_password = lv_textarea_create(pwd_row);
    lv_obj_set_size(ta_password, SCREEN_WIDTH - 54, 32);
    lv_obj_align(ta_password, LV_ALIGN_LEFT_MID, 0, 0);
    lv_textarea_set_placeholder_text(ta_password, "Nhập mật khẩu...");
    lv_textarea_set_password_mode(ta_password, true);
    lv_textarea_set_one_line(ta_password, true);
    lv_obj_set_style_bg_color(ta_password, lv_color_hex(0x131A26), 0);
    lv_obj_set_style_border_color(ta_password, lv_color_hex(0x2E3B52), 0);
    lv_obj_set_style_text_color(ta_password, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(ta_password, UI_FONT_12, 0);

    btn_eye = lv_btn_create(pwd_row);
    lv_obj_set_size(btn_eye, 34, 32);
    lv_obj_set_ext_click_area(btn_eye, 4);
    lv_obj_align(btn_eye, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(btn_eye, 6, 0);
    lv_obj_set_style_bg_color(btn_eye, lv_color_hex(0x1E293B), 0);
    lv_obj_add_event_cb(btn_eye, eye_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lbl_eye = lv_label_create(btn_eye);
    lv_label_set_text(lbl_eye, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(lbl_eye, UI_FONT_10, 0);
    lv_obj_center(lbl_eye);

    // Hàng nút thao tác: Kết nối / Quên mạng
    lv_obj_t *action_row = lv_obj_create(pwd_modal);
    lv_obj_set_size(action_row, SCREEN_WIDTH - 16, 32);
    lv_obj_align(action_row, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(action_row, 0, 0);
    lv_obj_set_style_pad_all(action_row, 0, 0);
    lv_obj_clear_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);

    btn_connect = lv_btn_create(action_row);
    lv_obj_set_size(btn_connect, 126, 32);
    lv_obj_set_ext_click_area(btn_connect, 4);
    lv_obj_align(btn_connect, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(btn_connect, 6, 0);
    lv_obj_set_style_bg_color(btn_connect, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_add_event_cb(btn_connect, connect_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_conn = lv_label_create(btn_connect);
    lv_label_set_text(lbl_conn, LV_SYMBOL_OK " Kết Nối");
    lv_obj_set_style_text_color(lbl_conn, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_text_font(lbl_conn, UI_FONT_12, 0);
    lv_obj_center(lbl_conn);

    btn_forget = lv_btn_create(action_row);
    lv_obj_set_size(btn_forget, 76, 32);
    lv_obj_set_ext_click_area(btn_forget, 4);
    lv_obj_align(btn_forget, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(btn_forget, 6, 0);
    lv_obj_set_style_bg_color(btn_forget, lv_color_hex(0x281B24), 0);
    lv_obj_set_style_border_color(btn_forget, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_border_width(btn_forget, 1, 0);
    lv_obj_add_event_cb(btn_forget, forget_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_f = lv_label_create(btn_forget);
    lv_label_set_text(lbl_f, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(lbl_f, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_text_font(lbl_f, UI_FONT_12, 0);
    lv_obj_center(lbl_f);

    // Nhãn trạng thái
    lbl_status = lv_label_create(pwd_modal);
    lv_label_set_text(lbl_status, "Vui lòng nhập mật khẩu");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_status, UI_FONT_10, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 98);

    // Bàn phím ảo LVGL tối ưu cho chiều rộng 240px
    keyboard = lv_keyboard_create(pwd_modal);
    lv_obj_set_size(keyboard, SCREEN_WIDTH - 16, 126);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, ta_password);
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_ALL, nullptr);

    // Kích hoạt quét WiFi ngay khi mở app
    scan_btn_event_cb(nullptr);
}

/* Đóng và dọn dẹp app */
void wifi_app_close(void)
{
    is_active = false;
    main_container = nullptr;
    top_bar = nullptr;
    btn_scan = nullptr;
    lbl_scan_info = nullptr;
    network_list = nullptr;
    pwd_modal = nullptr;
    lbl_target_ssid = nullptr;
    ta_password = nullptr;
    btn_eye = nullptr;
    lbl_eye = nullptr;
    btn_connect = nullptr;
    btn_cancel = nullptr;
    btn_forget = nullptr;
    lbl_status = nullptr;
    keyboard = nullptr;
}

/* Cập nhật định kỳ */
void wifi_app_update(void)
{
    if (!is_active || !network_list) return;

    // Cập nhật trạng thái quét
    if (wifi_manager_get_state() == WIFI_STATE_SCANNING || !wifi_manager_is_scan_done())
    {
        if (lbl_scan_info)
        {
            lv_label_set_text(lbl_scan_info, "Đang quét...");
            lv_obj_set_style_text_color(lbl_scan_info, lv_color_hex(COLOR_ACCENT_CYAN), 0);
        }
        need_list_refresh = true;
    }
    else
    {
        if (need_list_refresh)
        {
            need_list_refresh = false;

            cached_scan_results = wifi_manager_get_scan_results();
            lv_obj_clean(network_list);

            if (lbl_scan_info)
            {
                lv_label_set_text_fmt(lbl_scan_info, "Tìm thấy: %d", (int)cached_scan_results.size());
                lv_obj_set_style_text_color(lbl_scan_info, lv_color_hex(COLOR_ACCENT_GREEN), 0);
            }

            if (cached_scan_results.empty())
            {
                lv_obj_t *empty_lbl = lv_label_create(network_list);
                lv_label_set_text(empty_lbl, "Không tìm thấy mạng nào.");
                lv_obj_set_style_text_color(empty_lbl, lv_color_hex(COLOR_TEXT_MUTED), 0);
                lv_obj_set_style_text_font(empty_lbl, UI_FONT_10, 0);
            }
            else
            {
                for (size_t i = 0; i < cached_scan_results.size(); i++)
                {
                    const auto &net = cached_scan_results[i];

                    char safe_ssid[48];
                    sanitize_ssid(net.ssid, safe_ssid, sizeof(safe_ssid));

                    char item_text[72];
                    snprintf(item_text, sizeof(item_text), "%s (%ddBm)", safe_ssid, (int)net.rssi);

                    const char *sym = !net.is_encrypted ? LV_SYMBOL_WIFI : LV_SYMBOL_EYE_CLOSE;
                    lv_obj_t *btn = lv_list_add_btn(network_list, sym, item_text);
                    lv_obj_set_style_radius(btn, 6, 0);
                    lv_obj_set_style_bg_color(btn, lv_color_hex(0x151B27), 0);
                    lv_obj_set_style_pad_ver(btn, 8, 0);
                    lv_obj_set_style_pad_hor(btn, 6, 0);
                    lv_obj_set_style_text_color(btn, lv_color_hex(COLOR_TEXT_WHITE), 0);
                    lv_obj_set_style_text_font(btn, UI_FONT_12, 0);

                    // Ellipsis cho SSID dài
                    uint32_t child_cnt = lv_obj_get_child_cnt(btn);
                    if (child_cnt >= 2)
                    {
                        lv_obj_t *btn_lbl = lv_obj_get_child(btn, 1);
                        if (btn_lbl)
                        {
                            lv_label_set_long_mode(btn_lbl, LV_LABEL_LONG_DOT);
                            lv_obj_set_width(btn_lbl, SCREEN_WIDTH - 64);
                        }
                    }

                    // Callback khi chọn mạng
                    lv_obj_add_event_cb(btn, network_item_clicked_cb, LV_EVENT_CLICKED, (void *)net.ssid);
                }
            }
        }
    }

    // Cập nhật trạng thái kết nối
    if (lbl_status)
    {
        WiFiState state = wifi_manager_get_state();
        if (state == WIFI_STATE_CONNECTED)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), LV_SYMBOL_OK " Đã kết nối: %s", wifi_manager_get_ip().c_str());
            lv_label_set_text(lbl_status, buf);
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(COLOR_ACCENT_GREEN), 0);
        }
        else if (state == WIFI_STATE_FAILED)
        {
            lv_label_set_text(lbl_status, LV_SYMBOL_CLOSE " Kết nối thất bại!");
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(COLOR_ACCENT_RED), 0);
        }
    }
}
