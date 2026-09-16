/**
 * @file ai_voice_app.cpp
 * @brief Giao diện ứng dụng AI Voice Assistant trên LVGL 8
 * Khung hội thoại bong bóng chat Messenger/iMessage, nút Push-to-Talk và sóng âm Waveform
 */

#include "ai_voice_app.h"
#include "../ai/ai_voice_service.h"
#include "../audio/audio_manager.h"
#include "../ui/ui_theme.h"

#define NUM_WAVE_BARS 7

// Các thành phần widget giao diện
static lv_obj_t *main_container = nullptr;
static lv_obj_t *chat_container = nullptr;
static lv_obj_t *bottom_bar = nullptr;
static lv_obj_t *btn_push_to_talk = nullptr;
static lv_obj_t *lbl_ptt_icon = nullptr;
static lv_obj_t *lbl_status_text = nullptr;
static lv_obj_t *wave_bars[NUM_WAVE_BARS] = {nullptr};

static int last_msg_count = 0;
static bool is_button_held = false;

/* Tạo một bong bóng tin nhắn chat */
static void add_chat_bubble(const ChatMessage *msg)
{
    if (!chat_container || !msg) return;

    lv_obj_t *bubble = lv_obj_create(chat_container);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, 190, 0);
    lv_obj_set_style_pad_all(bubble, 6, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (msg->is_user)
    {
        // Bong bóng Người Dùng (Căn lề Phải, Màu Cyan)
        lv_obj_set_style_align(bubble, LV_ALIGN_TOP_RIGHT, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x0C2B3E), 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(0x00F2FE), 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_radius(bubble, 12, 0);

        lv_obj_t *lbl_text = lv_label_create(bubble);
        lv_label_set_text(lbl_text, msg->text);
        lv_label_set_long_mode(lbl_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl_text, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(lbl_text, 175, 0);
        lv_obj_set_style_text_color(lbl_text, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lbl_text, UI_FONT_12, 0);
    }
    else
    {
        // Bong bóng AI Assistant (Căn lề Trái, Màu Tím Neon)
        lv_obj_set_style_align(bubble, LV_ALIGN_TOP_LEFT, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x1C162E), 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(0x9D4EDD), 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_radius(bubble, 12, 0);

        // Header nhỏ: dịch vụ HTTPS đã cấu hình
        lv_obj_t *lbl_hdr = lv_label_create(bubble);
        lv_label_set_text(lbl_hdr, "AI Voice");
        lv_obj_set_style_text_color(lbl_hdr, lv_color_hex(0x00F2FE), 0);
        lv_obj_set_style_text_font(lbl_hdr, UI_FONT_SMALL, 0);
        lv_obj_align(lbl_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *lbl_text = lv_label_create(bubble);
        lv_label_set_text(lbl_text, msg->text);
        lv_label_set_long_mode(lbl_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl_text, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(lbl_text, 175, 0);
        lv_obj_set_style_text_color(lbl_text, lv_color_hex(0xE2E8F0), 0);
        lv_obj_set_style_text_font(lbl_text, UI_FONT_12, 0);
        lv_obj_set_style_pad_top(lbl_text, 14, 0);
    }

    // Tự động cuộn xuống tin nhắn mới nhất
    lv_obj_scroll_to_view(bubble, LV_ANIM_ON);
}

/* Callback xử lý sự kiện nút Push-to-Talk (Nhấn giữ để Nói, nhả ra để Gửi) */
static void ptt_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED)
    {
        if (ai_voice_start_recording())
        {
            is_button_held = true;
            // Hiệu ứng phát sáng mạnh khi đang giữ
            lv_obj_set_style_bg_color(btn_push_to_talk, lv_color_hex(0x00F2FE), 0);
            lv_obj_set_style_shadow_width(btn_push_to_talk, 18, 0);
            lv_obj_set_style_shadow_color(btn_push_to_talk, lv_color_hex(0x00F2FE), 0);
            lv_obj_set_style_shadow_opa(btn_push_to_talk, LV_OPA_80, 0);
            if (lbl_ptt_icon)
            {
                lv_obj_set_style_text_color(lbl_ptt_icon, lv_color_hex(0x0A0D14), 0);
            }
        }
        else
        {
            is_button_held = false;
            if (lbl_status_text)
            {
                lv_label_set_text(lbl_status_text, ai_voice_get_last_error());
            }
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        if (is_button_held)
        {
            is_button_held = false;
            if (!ai_voice_stop_and_process() && lbl_status_text)
                lv_label_set_text(lbl_status_text, ai_voice_get_last_error());

            // Trở về kiểu dáng bình thường
            lv_obj_set_style_bg_color(btn_push_to_talk, lv_color_hex(0x1F2937), 0);
            lv_obj_set_style_shadow_width(btn_push_to_talk, 6, 0);
            lv_obj_set_style_shadow_color(btn_push_to_talk, lv_color_hex(0x00F2FE), 0);
            lv_obj_set_style_shadow_opa(btn_push_to_talk, LV_OPA_30, 0);
            if (lbl_ptt_icon)
            {
                lv_obj_set_style_text_color(lbl_ptt_icon, lv_color_hex(0x00F2FE), 0);
            }
        }
    }
}

/* Mở ứng dụng AI Voice Assistant */
void ai_voice_app_open(lv_obj_t *parent)
{
    if (!parent) return;

    main_container = parent;
    lv_obj_set_style_pad_all(main_container, 0, 0);
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);

    // =========================================================================
    // =========================================================================
    // 1. KHUNG HỘI THOẠI BONG BÓNG CHAT
    // =========================================================================
    lv_coord_t b_bar_h = (DISP_VER_RES <= 240) ? 52 : 64;
    lv_coord_t chat_h = (DISP_VER_RES - 54) - b_bar_h;

    chat_container = lv_obj_create(main_container);
    lv_obj_set_size(chat_container, DISP_HOR_RES, chat_h);
    lv_obj_align(chat_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(chat_container, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_border_width(chat_container, 0, 0);
    lv_obj_set_style_radius(chat_container, 0, 0);
    lv_obj_set_style_pad_all(chat_container, 8, 0);
    lv_obj_set_flex_flow(chat_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_cross_place(chat_container, LV_FLEX_ALIGN_START, 0);

    // Nạp tất cả tin nhắn hiện có trong lịch sử
    int count = ai_voice_get_message_count();
    for (int i = 0; i < count; i++)
    {
        ChatMessage msg;
        if (ai_voice_get_message_copy(i, &msg)) add_chat_bubble(&msg);
    }
    last_msg_count = count;

    // =========================================================================
    // 2. KHUNG ĐIỀU KHIỂN ĐÁY: NÚT PUSH-TO-TALK, SÓNG ÂM VÀ STATUS
    // =========================================================================
    bottom_bar = lv_obj_create(main_container);
    lv_obj_set_size(bottom_bar, DISP_HOR_RES, b_bar_h);
    lv_obj_align(bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar, lv_color_hex(0x111622), 0);
    lv_obj_set_style_border_color(bottom_bar, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(bottom_bar, 1, 0);
    lv_obj_set_style_border_side(bottom_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_set_style_pad_hor(bottom_bar, 12, 0);
    lv_obj_set_style_pad_ver(bottom_bar, 4, 0);
    lv_obj_clear_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);

    // 2.1 Hiệu ứng dải sóng âm Waveform (7 thanh bar)
    lv_obj_t *wave_container = lv_obj_create(bottom_bar);
    lv_obj_set_size(wave_container, 56, 44);
    lv_obj_align(wave_container, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_opa(wave_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave_container, 0, 0);
    lv_obj_set_style_pad_all(wave_container, 0, 0);
    lv_obj_clear_flag(wave_container, LV_OBJ_FLAG_SCROLLABLE);

    int bar_x_coords[] = { 2, 10, 18, 26, 34, 42, 50 };
    for (int i = 0; i < NUM_WAVE_BARS; i++)
    {
        wave_bars[i] = lv_obj_create(wave_container);
        lv_obj_set_size(wave_bars[i], 4, 6);
        lv_obj_set_pos(wave_bars[i], bar_x_coords[i], 19);
        lv_obj_set_style_radius(wave_bars[i], 2, 0);
        lv_obj_set_style_bg_color(wave_bars[i], (i % 2 == 0) ? lv_color_hex(0x00F2FE) : lv_color_hex(0x9D4EDD), 0);
        lv_obj_set_style_border_width(wave_bars[i], 0, 0);
        lv_obj_clear_flag(wave_bars[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    // 2.2 Nút tròn to Push-to-Talk (44x44) ở trung tâm
    btn_push_to_talk = lv_btn_create(bottom_bar);
    lv_obj_set_size(btn_push_to_talk, 44, 44);
    lv_obj_align(btn_push_to_talk, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(btn_push_to_talk, 22, 0);
    lv_obj_set_style_bg_color(btn_push_to_talk, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_color(btn_push_to_talk, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_border_width(btn_push_to_talk, 2, 0);
    lv_obj_set_style_shadow_width(btn_push_to_talk, 6, 0);
    lv_obj_set_style_shadow_color(btn_push_to_talk, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_shadow_opa(btn_push_to_talk, LV_OPA_40, 0);
    lv_obj_add_event_cb(btn_push_to_talk, ptt_btn_event_cb, LV_EVENT_ALL, nullptr);

    lbl_ptt_icon = lv_label_create(btn_push_to_talk);
    lv_label_set_text(lbl_ptt_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(lbl_ptt_icon, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_text_font(lbl_ptt_icon, UI_FONT_14, 0);
    lv_obj_center(lbl_ptt_icon);

    // 2.3 Nhãn trạng thái AI & Hướng dẫn sử dụng
    lbl_status_text = lv_label_create(bottom_bar);
    lv_label_set_text(lbl_status_text, ai_voice_get_state_text());
    lv_obj_set_style_text_color(lbl_status_text, lv_color_hex(0xA0AEC0), 0);
    lv_obj_set_style_text_font(lbl_status_text, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_status_text, LV_ALIGN_RIGHT_MID, -4, 0);

    if (!ai_voice_is_available())
    {
        lv_obj_add_state(btn_push_to_talk, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(lbl_status_text, lv_color_hex(0xFF5252), 0);
    }
}

/* Đóng và giải phóng tài nguyên */
void ai_voice_app_close(void)
{
    ai_voice_cancel();
    is_button_held = false;
    main_container = nullptr;
    chat_container = nullptr;
    bottom_bar = nullptr;
    btn_push_to_talk = nullptr;
    lbl_ptt_icon = nullptr;
    lbl_status_text = nullptr;
    for (int i = 0; i < NUM_WAVE_BARS; i++)
    {
        wave_bars[i] = nullptr;
    }
}

/* Cập nhật định kỳ tin nhắn mới và animation sóng âm */
void ai_voice_app_update(void)
{
    if (!main_container) return;

    // 1. Kiểm tra và thêm tin nhắn mới vào khung chat nếu có
    int current_count = ai_voice_get_message_count();
    if (current_count > last_msg_count)
    {
        for (int i = last_msg_count; i < current_count; i++)
        {
            ChatMessage msg;
            if (ai_voice_get_message_copy(i, &msg))
            {
                add_chat_bubble(&msg);
            }
        }
        last_msg_count = current_count;
    }

    // 2. Cập nhật nhãn trạng thái AI
    if (lbl_status_text)
    {
        lv_label_set_text(lbl_status_text, ai_voice_get_state_text());
        AIVoiceState state = ai_voice_get_state();
        if (state == AI_STATE_LISTENING)
            lv_obj_set_style_text_color(lbl_status_text, lv_color_hex(0x00F2FE), 0);
        else if (state == AI_STATE_PROCESSING)
            lv_obj_set_style_text_color(lbl_status_text, lv_color_hex(0xFFB300), 0);
        else if (state == AI_STATE_SPEAKING)
            lv_obj_set_style_text_color(lbl_status_text, lv_color_hex(0x00E676), 0);
        else
            lv_obj_set_style_text_color(lbl_status_text, lv_color_hex(0xA0AEC0), 0);
    }

    // 3. Hiển thị biên độ microphone thật; không tạo hoạt ảnh giả khi không thu.
    AIVoiceState state = ai_voice_get_state();
    int16_t samples[NUM_WAVE_BARS] = {};
    if (state == AI_STATE_LISTENING) audio_get_waveform_samples(samples, NUM_WAVE_BARS);

    for (int i = 0; i < NUM_WAVE_BARS; i++)
    {
        if (wave_bars[i])
        {
            int h = 6;
            if (state == AI_STATE_LISTENING)
            {
                int32_t amplitude = samples[i] < 0 ? -(int32_t)samples[i] : samples[i];
                h = 6 + (amplitude * 30 / 32767);
                if (h > 36) h = 36;
            }
            lv_obj_set_height(wave_bars[i], h);
            lv_obj_set_y(wave_bars[i], 24 - (h / 2));
        }
    }
}
