/**
 * @file wifi_app.cpp
 * @brief Triển khai module WiFi Settings App trên LVGL 8 (Màn hình 3.5" 480x320)
 * Bố cục 2 nửa: Quét mạng (trái ~220px) và Bàn phím ảo cảm ứng / Nhập mật khẩu (phải ~242px)
 * Lưu trữ NVS Flash vĩnh viễn qua Preferences.h
 */

#include "wifi_app.h"
#include "../os/wifi_manager.h"
#include <stdio.h>
#include <string.h>

// Quản lý trạng thái giao diện
static lv_obj_t *main_container = nullptr;
static lv_obj_t *card_left = nullptr;
static lv_obj_t *card_right = nullptr;

// Thành phần nửa bên trái (Scanner & Network List)
static lv_obj_t *btn_scan = nullptr;
static lv_obj_t *lbl_scan_info = nullptr;
static lv_obj_t *network_list = nullptr;

// Thành phần nửa bên phải (Textarea, Keyboard, Status)
static lv_obj_t *lbl_target_ssid = nullptr;
static lv_obj_t *ta_password = nullptr;
static lv_obj_t *btn_eye = nullptr;
static lv_obj_t *lbl_eye = nullptr;
static lv_obj_t *btn_connect = nullptr;
static lv_obj_t *btn_forget = nullptr;
static lv_obj_t *lbl_status = nullptr;
static lv_obj_t *keyboard = nullptr;

// Bộ nhớ đệm dữ liệu người dùng
static char current_selected_ssid[33] = {0};
static bool is_pwd_visible = false;
static bool is_active = false;
static bool need_list_refresh = false;

/* =========================================================================
 * CÁC HÀM XỬ LÝ SỰ KIỆN (EVENT CALLBACKS)
 * ========================================================================= */

// Bấm nút Quét Mạng
static void scan_btn_event_cb(lv_event_t *e)
{
    if (lbl_scan_info)
    {
        lv_label_set_text(lbl_scan_info, "Đang quét sóng...");
        lv_obj_set_style_text_color(lbl_scan_info, lv_color_hex(0x00F2FE), 0);
    }
    wifi_manager_scan_async();
}

// Bấm chọn một mạng trong danh sách
static void network_item_clicked_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid || strlen(ssid) == 0) return;

    strncpy(current_selected_ssid, ssid, sizeof(current_selected_ssid) - 1);
    current_selected_ssid[sizeof(current_selected_ssid) - 1] = '\0';

    if (lbl_target_ssid)
    {
        lv_label_set_text_fmt(lbl_target_ssid, "Mạng: %s", current_selected_ssid);
    }

    if (ta_password)
    {
        lv_textarea_set_text(ta_password, "");
        lv_obj_clear_state(ta_password, LV_STATE_FOCUSED);
        lv_obj_add_state(ta_password, LV_STATE_FOCUSED);
    }

    if (lbl_status)
    {
        lv_label_set_text(lbl_status, "⚪ Vui lòng nhập mật khẩu");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xA0AEC0), 0);
    }
}

// Bấm nút Kết Nối
static void connect_btn_event_cb(lv_event_t *e)
{
    if (strlen(current_selected_ssid) == 0)
    {
        if (lbl_status)
        {
            lv_label_set_text(lbl_status, "⚠️ Hãy chọn một mạng bên trái!");
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFFB300), 0);
        }
        return;
    }

    const char *pwd = "";
    if (ta_password)
    {
        pwd = lv_textarea_get_text(ta_password);
    }

    if (lbl_status)
    {
        lv_label_set_text_fmt(lbl_status, "🟡 Đang kết nối tới %s...", current_selected_ssid);
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFFB300), 0);
    }

    // Gửi lệnh kết nối tới Core 0 Service (tự động lưu vào NVS Flash nếu thành công)
    wifi_manager_connect(current_selected_ssid, pwd);
}

// Bấm nút Quên Mạng (Xóa NVS và chặn auto-reconnect)
static void forget_btn_event_cb(lv_event_t *e)
{
    wifi_manager_forget_network();

    if (lbl_status)
    {
        lv_label_set_text(lbl_status, "🗑 Đã quên mạng khỏi hệ thống");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFF3B30), 0);
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
        // Khi người dùng bấm phím Check/Enter trên bàn phím ảo -> Tự động kích hoạt Kết Nối
        connect_btn_event_cb(nullptr);
    }
    else if (code == LV_EVENT_CANCEL)
    {
        // Bấm phím Cancel -> Thoát focus
        if (ta_password) lv_obj_clear_state(ta_password, LV_STATE_FOCUSED);
    }
}

/* =========================================================================
 * XÂY DỰNG GIAO DIỆN CHIA 2 NỬA (480x266 CHO 3.5" IPS)
 * ========================================================================= */
void wifi_app_open(lv_obj_t *parent)
{
    main_container = parent;
    is_active = true;

    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A0D14), 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // -------------------------------------------------------------------------
    // 1. NỬA BÊN TRÁI (W: 220px, H: 254px): DANH SÁCH MẠNG & NÚT REFRESH
    // -------------------------------------------------------------------------
    card_left = lv_obj_create(parent);
    lv_obj_set_size(card_left, 220, 254);
    lv_obj_set_pos(card_left, 2, 2);
    lv_obj_set_style_radius(card_left, 10, 0);
    lv_obj_set_style_bg_color(card_left, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(card_left, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_border_width(card_left, 1, 0);
    lv_obj_set_style_pad_all(card_left, 6, 0);
    lv_obj_clear_flag(card_left, LV_OBJ_FLAG_SCROLLABLE);

    // Thanh công cụ trên cùng: Nút Quét Lại & Nhãn số lượng mạng
    lv_obj_t *top_left_bar = lv_obj_create(card_left);
    lv_obj_set_size(top_left_bar, 208, 30);
    lv_obj_align(top_left_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top_left_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_left_bar, 0, 0);
    lv_obj_set_style_pad_all(top_left_bar, 0, 0);
    lv_obj_clear_flag(top_left_bar, LV_OBJ_FLAG_SCROLLABLE);

    btn_scan = lv_btn_create(top_left_bar);
    lv_obj_set_size(btn_scan, 98, 26);
    lv_obj_align(btn_scan, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(btn_scan, 6, 0);
    lv_obj_set_style_bg_color(btn_scan, lv_color_hex(0x00E676), 0);
    lv_obj_add_event_cb(btn_scan, scan_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_scan_btn = lv_label_create(btn_scan);
    lv_label_set_text(lbl_scan_btn, LV_SYMBOL_REFRESH " Quét Lại");
    lv_obj_set_style_text_color(lbl_scan_btn, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_text_font(lbl_scan_btn, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl_scan_btn);

    lbl_scan_info = lv_label_create(top_left_bar);
    lv_label_set_text(lbl_scan_info, "Đang sẵn sàng");
    lv_obj_set_style_text_color(lbl_scan_info, lv_color_hex(0xA0AEC0), 0);
    lv_obj_set_style_text_font(lbl_scan_info, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_scan_info, LV_ALIGN_RIGHT_MID, -2, 0);

    // Danh sách mạng WiFi (lv_list)
    network_list = lv_list_create(card_left);
    lv_obj_set_size(network_list, 208, 206);
    lv_obj_align(network_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(network_list, 8, 0);
    lv_obj_set_style_bg_color(network_list, lv_color_hex(0x111622), 0);
    lv_obj_set_style_border_color(network_list, lv_color_hex(0x2D3748), 0);
    lv_obj_set_style_border_width(network_list, 1, 0);
    lv_obj_set_style_pad_all(network_list, 2, 0);

    // -------------------------------------------------------------------------
    // 2. NỬA BÊN PHẢI (W: 242px, H: 254px): Ô MẬT KHẨU, BÀN PHÍM CẢM ỨNG & STATUS
    // -------------------------------------------------------------------------
    card_right = lv_obj_create(parent);
    lv_obj_set_size(card_right, 244, 254);
    lv_obj_set_pos(card_right, 228, 2);
    lv_obj_set_style_radius(card_right, 10, 0);
    lv_obj_set_style_bg_color(card_right, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(card_right, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_border_width(card_right, 1, 0);
    lv_obj_set_style_pad_all(card_right, 6, 0);
    lv_obj_clear_flag(card_right, LV_OBJ_FLAG_SCROLLABLE);

    // Hàng 1: Tên mạng SSID đã chọn
    lbl_target_ssid = lv_label_create(card_right);
    if (strlen(current_selected_ssid) > 0)
    {
        lv_label_set_text_fmt(lbl_target_ssid, "Mạng: %s", current_selected_ssid);
    }
    else
    {
        lv_label_set_text(lbl_target_ssid, "Mạng: Chưa chọn mạng");
    }
    lv_obj_set_style_text_color(lbl_target_ssid, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_text_font(lbl_target_ssid, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_target_ssid, LV_ALIGN_TOP_LEFT, 2, 0);

    // Hàng 2: Hộp nhập mật khẩu (Textarea) + Nút ẩn/hiện mắt
    ta_password = lv_textarea_create(card_right);
    lv_obj_set_size(ta_password, 194, 30);
    lv_obj_set_pos(ta_password, 2, 22);
    lv_textarea_set_password_mode(ta_password, true);
    lv_textarea_set_one_line(ta_password, true);
    lv_textarea_set_placeholder_text(ta_password, "Nhập mật khẩu...");
    lv_obj_set_style_radius(ta_password, 6, 0);
    lv_obj_set_style_bg_color(ta_password, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_border_color(ta_password, lv_color_hex(0x2D3748), 0);
    lv_obj_set_style_text_color(ta_password, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ta_password, &lv_font_montserrat_10, 0);

    btn_eye = lv_btn_create(card_right);
    lv_obj_set_size(btn_eye, 32, 30);
    lv_obj_set_pos(btn_eye, 198, 22);
    lv_obj_set_style_radius(btn_eye, 6, 0);
    lv_obj_set_style_bg_color(btn_eye, lv_color_hex(0x2D3748), 0);
    lv_obj_add_event_cb(btn_eye, eye_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lbl_eye = lv_label_create(btn_eye);
    lv_label_set_text(lbl_eye, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(lbl_eye, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl_eye);

    // Hàng 3: Hai nút Kết Nối và Quên Mạng
    btn_connect = lv_btn_create(card_right);
    lv_obj_set_size(btn_connect, 112, 26);
    lv_obj_set_pos(btn_connect, 2, 56);
    lv_obj_set_style_radius(btn_connect, 6, 0);
    lv_obj_set_style_bg_color(btn_connect, lv_color_hex(0x00E676), 0);
    lv_obj_add_event_cb(btn_connect, connect_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_conn_txt = lv_label_create(btn_connect);
    lv_label_set_text(lbl_conn_txt, LV_SYMBOL_OK " Kết Nối");
    lv_obj_set_style_text_color(lbl_conn_txt, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_text_font(lbl_conn_txt, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl_conn_txt);

    btn_forget = lv_btn_create(card_right);
    lv_obj_set_size(btn_forget, 112, 26);
    lv_obj_set_pos(btn_forget, 118, 56);
    lv_obj_set_style_radius(btn_forget, 6, 0);
    lv_obj_set_style_bg_color(btn_forget, lv_color_hex(0x7C2D12), 0);
    lv_obj_add_event_cb(btn_forget, forget_btn_event_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_forget_txt = lv_label_create(btn_forget);
    lv_label_set_text(lbl_forget_txt, LV_SYMBOL_TRASH " Quên NVS");
    lv_obj_set_style_text_color(lbl_forget_txt, lv_color_hex(0xFECACA), 0);
    lv_obj_set_style_text_font(lbl_forget_txt, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl_forget_txt);

    // Hàng 4: Nhãn trạng thái hiển thị (Đang kết nối, Thành công, Sai mật khẩu)
    lbl_status = lv_label_create(card_right);
    lv_obj_set_size(lbl_status, 230, 16);
    lv_obj_set_pos(lbl_status, 2, 84);
    if (wifi_manager_is_connected())
    {
        lv_label_set_text_fmt(lbl_status, "🟢 Đã kết nối: %s (%s)", wifi_manager_get_ssid().c_str(), wifi_manager_get_ip().c_str());
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x00E676), 0);
    }
    else
    {
        lv_label_set_text(lbl_status, "⚪ Chưa kết nối Internet");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xA0AEC0), 0);
    }
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_10, 0);

    // Hàng 5: Bàn phím ảo cảm ứng LVGL (230x140px)
    keyboard = lv_keyboard_create(card_right);
    lv_obj_set_size(keyboard, 232, 136);
    lv_obj_set_pos(keyboard, 0, 104);
    lv_obj_set_style_radius(keyboard, 6, 0);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(0x111622), 0);
    lv_obj_set_style_border_color(keyboard, lv_color_hex(0x2D3748), 0);
    lv_obj_set_style_border_width(keyboard, 1, 0);
    lv_keyboard_set_textarea(keyboard, ta_password);
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_ALL, nullptr);

    // Kích hoạt quét sóng tự động ngay khi mở app
    wifi_manager_scan_async();
}

void wifi_app_close(void)
{
    is_active = false;
    main_container = nullptr;
    card_left = nullptr;
    card_right = nullptr;
    btn_scan = nullptr;
    lbl_scan_info = nullptr;
    network_list = nullptr;
    lbl_target_ssid = nullptr;
    ta_password = nullptr;
    btn_eye = nullptr;
    lbl_eye = nullptr;
    btn_connect = nullptr;
    btn_forget = nullptr;
    lbl_status = nullptr;
    keyboard = nullptr;
}

bool wifi_app_is_active(void)
{
    return is_active;
}

void wifi_app_init(void)
{
    // Đã được cấu hình cùng hệ thống
}

/* =========================================================================
 * CẬP NHẬT ĐỊNH KỲ THỜI GIAN THỰC (TỪ OS LOOP)
 * ========================================================================= */
void wifi_app_update(void)
{
    if (!is_active) return;

    // 1. Cập nhật danh sách mạng khi quá trình quét hoàn tất
    if (wifi_manager_is_scan_done() && network_list)
    {
        std::vector<WiFiNetworkInfo> networks = wifi_manager_get_scan_results();
        lv_obj_clean(network_list);

        if (lbl_scan_info)
        {
            lv_label_set_text_fmt(lbl_scan_info, "Tìm thấy: %u mạng", networks.size());
            lv_obj_set_style_text_color(lbl_scan_info, lv_color_hex(0x00E676), 0);
        }

        String current_connected = wifi_manager_get_ssid();

        for (size_t i = 0; i < networks.size(); i++)
        {
            char btn_text[64];
            const char *lock_icon = networks[i].is_encrypted ? LV_SYMBOL_SETTINGS : "  ";
            snprintf(btn_text, sizeof(btn_text), "%s %-14.14s [%ddBm]",
                     networks[i].is_encrypted ? "🔒" : "🌐",
                     networks[i].ssid,
                     networks[i].rssi);

            lv_obj_t *item = lv_list_add_btn(network_list, LV_SYMBOL_WIFI, btn_text);
            lv_obj_set_style_radius(item, 4, 0);
            lv_obj_set_style_pad_ver(item, 4, 0);
            lv_obj_set_style_text_font(item, &lv_font_montserrat_10, 0);

            // Nếu đây là mạng đang kết nối hiện tại -> làm nổi bật viền xanh lá
            if (current_connected.length() > 0 && current_connected.equals(networks[i].ssid))
            {
                lv_obj_set_style_bg_color(item, lv_color_hex(0x00E676), 0);
                lv_obj_set_style_bg_opa(item, LV_OPA_30, 0);
                lv_obj_set_style_border_color(item, lv_color_hex(0x00E676), 0);
                lv_obj_set_style_border_width(item, 1, 0);
            }
            else
            {
                lv_obj_set_style_bg_color(item, lv_color_hex(0x161B26), 0);
                lv_obj_set_style_border_color(item, lv_color_hex(0x2D3748), 0);
                lv_obj_set_style_border_width(item, 1, 0);
            }

            // Gán con trỏ SSID vào user data
            lv_obj_add_event_cb(item, network_item_clicked_cb, LV_EVENT_CLICKED, (void *)networks[i].ssid);
        }

        if (networks.empty())
        {
            lv_obj_t *lbl_none = lv_label_create(network_list);
            lv_label_set_text(lbl_none, "Không tìm thấy mạng!\nHãy bấm Quét Lại.");
            lv_obj_set_style_text_color(lbl_none, lv_color_hex(0x718096), 0);
            lv_obj_set_style_text_font(lbl_none, &lv_font_montserrat_10, 0);
            lv_obj_center(lbl_none);
        }
    }

    // 2. Cập nhật nhãn trạng thái kết nối
    if (lbl_status)
    {
        WiFiState state = wifi_manager_get_state();
        switch (state)
        {
            case WIFI_STATE_CONNECTING:
                lv_label_set_text(lbl_status, "🟡 Đang kết nối...");
                lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFFB300), 0);
                break;
            case WIFI_STATE_CONNECTED:
                lv_label_set_text_fmt(lbl_status, "🟢 Thành công! (IP: %s)", wifi_manager_get_ip().c_str());
                lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x00E676), 0);
                break;
            case WIFI_STATE_FAILED:
                lv_label_set_text(lbl_status, "🔴 Sai mật khẩu hoặc kết nối lỗi!");
                lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFF3B30), 0);
                break;
            case WIFI_STATE_DISCONNECTED:
                // Nếu không ở các trạng thái trên, giữ nhãn hiện tại
                break;
            default:
                break;
        }
    }
}
