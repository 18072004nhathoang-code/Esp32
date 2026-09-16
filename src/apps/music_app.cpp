/**
 * @file music_app.cpp
 * @brief Triển khai giao diện Music Player responsive trên LVGL 8.
 * Bố cục 1 cột dọc: Artwork đĩa than xoay ở trên, tên bài hát + thanh tiến trình + điều khiển cảm ứng
 */

#include "music_app.h"
#include "../audio/music_player.h"
#include "../ui/ui_theme.h"

// Các thành phần widget giao diện
static lv_obj_t *main_container = nullptr;
static lv_obj_t *player_card = nullptr;
static lv_obj_t *playlist_modal = nullptr;
static lv_obj_t *music_list = nullptr;
static lv_obj_t *vinyl_disc = nullptr;
static lv_obj_t *lbl_track_title = nullptr;
static lv_obj_t *lbl_track_meta = nullptr;
static lv_obj_t *slider_progress = nullptr;
static lv_obj_t *lbl_cur_time = nullptr;
static lv_obj_t *lbl_total_time = nullptr;
static lv_obj_t *btn_play = nullptr;
static lv_obj_t *lbl_play_icon = nullptr;
static lv_obj_t *slider_volume = nullptr;
static lv_obj_t *lbl_vol_val = nullptr;

static lv_anim_t vinyl_anim;
static bool vinyl_anim_running = false;
static int32_t current_vinyl_angle = 0;
static bool is_user_dragging_slider = false;

/* Callback cập nhật góc xoay cho đĩa than Vinyl */
static void anim_vinyl_rotate_cb(void *var, int32_t v)
{
    if (vinyl_disc)
    {
        current_vinyl_angle = v;
        lv_obj_set_style_transform_angle(vinyl_disc, (int16_t)v, 0);
    }
}

/* Callback khi bấm vào bài hát trong danh sách playlist */
static void track_item_click_cb(lv_event_t *e)
{
    uintptr_t track_idx = (uintptr_t)lv_event_get_user_data(e);
    music_player_play_index((int)track_idx);
    if (playlist_modal)
    {
        lv_obj_add_flag(playlist_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Callback bấm nút Play / Pause */
static void play_btn_click_cb(lv_event_t *e)
{
    music_player_toggle_play();
}

/* Callback bấm nút Next */
static void next_btn_click_cb(lv_event_t *e)
{
    music_player_next();
}

/* Callback bấm nút Previous */
static void prev_btn_click_cb(lv_event_t *e)
{
    music_player_prev();
}

/* Callback mở / đóng danh sách phát nhạc */
static void playlist_toggle_cb(lv_event_t *e)
{
    if (!playlist_modal) return;
    if (lv_obj_has_flag(playlist_modal, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_clear_flag(playlist_modal, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(playlist_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Callback thanh trượt tua bài hát (Seek) */
static void progress_slider_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED)
    {
        is_user_dragging_slider = true;
    }
    else if (code == LV_EVENT_RELEASED)
    {
        is_user_dragging_slider = false;
        int val = lv_slider_get_value(slider_progress);
        music_player_seek((uint32_t)val);
    }
}

/* Callback thanh trượt âm lượng (Volume) */
static void volume_slider_event_cb(lv_event_t *e)
{
    int val = lv_slider_get_value(slider_volume);
    music_player_set_volume((uint8_t)val);
    if (lbl_vol_val)
    {
        lv_label_set_text_fmt(lbl_vol_val, "%d%%", val);
    }
}

/* Mở ứng dụng Music Player theo logical display hiện tại. */
void music_app_open(lv_obj_t *parent)
{
    if (!parent) return;

    main_container = parent;
    const bool portrait = SCREEN_WIDTH <= SCREEN_HEIGHT;
    const lv_coord_t detail_w = portrait ? (SCREEN_WIDTH - 28) : 186;
    lv_obj_set_style_pad_all(main_container, 4, 0);
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);

    // Card chính bao trọn khung dọc/ngang
    player_card = lv_obj_create(main_container);
    lv_obj_set_size(player_card, SCREEN_WIDTH - 8, APP_CONTENT_HEIGHT - 6);
    lv_obj_center(player_card);
    lv_obj_set_style_radius(player_card, 14, 0);
    lv_obj_set_style_bg_color(player_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(player_card, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_width(player_card, 1, 0);
    lv_obj_set_style_pad_all(player_card, 6, 0);
    lv_obj_clear_flag(player_card, LV_OBJ_FLAG_SCROLLABLE);

    // 1. ARTWORK ĐĨA THAN QUAY BÊN TRÁI (Size 88x88)
    vinyl_disc = lv_obj_create(player_card);
    lv_obj_set_size(vinyl_disc, portrait ? 76 : 88, portrait ? 76 : 88);
    lv_obj_align(vinyl_disc, portrait ? LV_ALIGN_TOP_MID : LV_ALIGN_LEFT_MID,
                 portrait ? 0 : 8, portrait ? 2 : 0);
    lv_obj_set_style_radius(vinyl_disc, portrait ? 38 : 44, 0);
    lv_obj_set_style_bg_color(vinyl_disc, lv_color_hex(0x0C0E14), 0);
    lv_obj_set_style_border_color(vinyl_disc, lv_color_hex(0x2A3346), 0);
    lv_obj_set_style_border_width(vinyl_disc, 3, 0);
    lv_obj_clear_flag(vinyl_disc, LV_OBJ_FLAG_SCROLLABLE);

    // Vòng rãnh đĩa than
    lv_obj_t *groove = lv_obj_create(vinyl_disc);
    lv_obj_set_size(groove, portrait ? 52 : 60, portrait ? 52 : 60);
    lv_obj_center(groove);
    lv_obj_set_style_radius(groove, portrait ? 26 : 30, 0);
    lv_obj_set_style_bg_opa(groove, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(groove, lv_color_hex(0x1F2837), 0);
    lv_obj_set_style_border_width(groove, 1, 0);
    lv_obj_clear_flag(groove, LV_OBJ_FLAG_SCROLLABLE);

    // Tâm nhãn đĩa màu Tím Neon
    lv_obj_t *vinyl_label = lv_obj_create(vinyl_disc);
    lv_obj_set_size(vinyl_label, 32, 32);
    lv_obj_center(vinyl_label);
    lv_obj_set_style_radius(vinyl_label, 16, 0);
    lv_obj_set_style_bg_color(vinyl_label, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_color(vinyl_label, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_border_width(vinyl_label, 2, 0);
    lv_obj_clear_flag(vinyl_label, LV_OBJ_FLAG_SCROLLABLE);

    // Lỗ tâm trục đĩa
    lv_obj_t *spindle_hole = lv_obj_create(vinyl_label);
    lv_obj_set_size(spindle_hole, 8, 8);
    lv_obj_center(spindle_hole);
    lv_obj_set_style_radius(spindle_hole, 4, 0);
    lv_obj_set_style_bg_color(spindle_hole, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_border_width(spindle_hole, 0, 0);
    lv_obj_clear_flag(spindle_hole, LV_OBJ_FLAG_SCROLLABLE);

    // Animation xoay tròn
    lv_anim_init(&vinyl_anim);
    lv_anim_set_var(&vinyl_anim, vinyl_disc);
    lv_anim_set_values(&vinyl_anim, 0, 3600);
    lv_anim_set_time(&vinyl_anim, 3000);
    lv_anim_set_repeat_count(&vinyl_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&vinyl_anim, anim_vinyl_rotate_cb);
    vinyl_anim_running = false;

    // 2. CỘT PHẢI: TÊN BÀI HÁT & THÔNG TIN (X = 106)
    lbl_track_title = lv_label_create(player_card);
    lv_label_set_long_mode(lbl_track_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_track_title, detail_w);
    lv_obj_align(lbl_track_title, portrait ? LV_ALIGN_TOP_MID : LV_ALIGN_TOP_RIGHT,
                 portrait ? 0 : -6, portrait ? 82 : 2);
    lv_obj_set_style_text_align(lbl_track_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(lbl_track_title, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_track_title, UI_FONT_TITLE, 0);

    const MusicTrack *cur_track = music_player_get_track(music_player_get_current_index());
    if (cur_track)
    {
        lv_label_set_text(lbl_track_title, cur_track->title);
    }
    else
    {
        lv_label_set_text(lbl_track_title, "Chưa chọn bài hát");
    }

    lbl_track_meta = lv_label_create(player_card);
    lv_label_set_text_fmt(lbl_track_meta, "SD Card MP3 • %d bài", music_player_get_track_count());
    lv_obj_set_style_text_color(lbl_track_meta, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_track_meta, UI_FONT_12, 0);
    lv_obj_set_width(lbl_track_meta, detail_w);
    lv_obj_align(lbl_track_meta, portrait ? LV_ALIGN_TOP_MID : LV_ALIGN_TOP_RIGHT,
                 portrait ? 0 : -6, portrait ? 108 : 24);

    // 3. THANH TIẾN TRÌNH (SEEK SLIDER) VÀ THỜI GIAN
    slider_progress = lv_slider_create(player_card);
    lv_obj_set_size(slider_progress, detail_w, 6);
    lv_obj_align(slider_progress, portrait ? LV_ALIGN_TOP_MID : LV_ALIGN_TOP_RIGHT,
                 portrait ? 0 : -6, portrait ? 132 : 44);
    lv_slider_set_range(slider_progress, 0, 100);
    lv_slider_set_value(slider_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_progress, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_progress, lv_color_hex(COLOR_ACCENT_PURPLE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_progress, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_progress, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(slider_progress, progress_slider_event_cb, LV_EVENT_ALL, nullptr);

    // Thời gian hiện tại (Trái cột phải) & Tổng thời lượng (Phải cột phải)
    lbl_cur_time = lv_label_create(player_card);
    lv_label_set_text(lbl_cur_time, "00:00");
    lv_obj_set_style_text_color(lbl_cur_time, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_cur_time, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_cur_time, portrait ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT,
                 portrait ? 4 : -152, portrait ? 142 : 54);

    lbl_total_time = lv_label_create(player_card);
    lv_label_set_text(lbl_total_time, "00:00");
    lv_obj_set_style_text_color(lbl_total_time, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_total_time, UI_FONT_SMALL, 0);
    lv_obj_align(lbl_total_time, LV_ALIGN_TOP_RIGHT, portrait ? -4 : -6, portrait ? 142 : 54);

    // 4. HÀNG ĐIỀU KHIỂN CẢM ỨNG: PREV - PLAY/PAUSE - NEXT (Nút tối thiểu >= 36px)
    lv_obj_t *ctrl_row = lv_obj_create(player_card);
    lv_obj_set_size(ctrl_row, detail_w, 48);
    lv_obj_align(ctrl_row, portrait ? LV_ALIGN_TOP_MID : LV_ALIGN_TOP_RIGHT,
                 portrait ? 0 : -6, portrait ? 160 : 72);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);
    lv_obj_clear_flag(ctrl_row, LV_OBJ_FLAG_SCROLLABLE);

    // Nút Previous
    lv_obj_t *btn_prev = lv_btn_create(ctrl_row);
    lv_obj_set_size(btn_prev, 36, 36);
    lv_obj_align(btn_prev, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_radius(btn_prev, 18, 0);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x1F2837), 0);
    lv_obj_set_style_border_color(btn_prev, lv_color_hex(0x374151), 0);
    lv_obj_set_style_border_width(btn_prev, 1, 0);
    lv_obj_add_event_cb(btn_prev, prev_btn_click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(lbl_prev, UI_FONT_12, 0);
    lv_obj_center(lbl_prev);

    // Nút Play/Pause chính (Nổi bật 44x44)
    btn_play = lv_btn_create(ctrl_row);
    lv_obj_set_size(btn_play, 44, 44);
    lv_obj_align(btn_play, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(btn_play, 22, 0);
    lv_obj_set_style_bg_color(btn_play, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_shadow_width(btn_play, 8, 0);
    lv_obj_set_style_shadow_color(btn_play, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_shadow_opa(btn_play, LV_OPA_50, 0);
    lv_obj_add_event_cb(btn_play, play_btn_click_cb, LV_EVENT_CLICKED, nullptr);

    lbl_play_icon = lv_label_create(btn_play);
    lv_label_set_text(lbl_play_icon, music_player_is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(lbl_play_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_play_icon, UI_FONT_14, 0);
    lv_obj_center(lbl_play_icon);

    // Nút Next
    lv_obj_t *btn_next = lv_btn_create(ctrl_row);
    lv_obj_set_size(btn_next, 36, 36);
    lv_obj_align(btn_next, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_radius(btn_next, 18, 0);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x1F2837), 0);
    lv_obj_set_style_border_color(btn_next, lv_color_hex(0x374151), 0);
    lv_obj_set_style_border_width(btn_next, 1, 0);
    lv_obj_add_event_cb(btn_next, next_btn_click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(lbl_next, UI_FONT_12, 0);
    lv_obj_center(lbl_next);

    // 5. HÀNG DƯỚI CỘT PHẢI: THANH ÂM LƯỢNG & NÚT MỞ PLAYLIST
    lv_obj_t *bottom_row = lv_obj_create(player_card);
    lv_obj_set_size(bottom_row, detail_w, 34);
    lv_obj_align(bottom_row, portrait ? LV_ALIGN_TOP_MID : LV_ALIGN_TOP_RIGHT,
                 portrait ? 0 : -6, portrait ? 216 : 126);
    lv_obj_set_style_bg_color(bottom_row, lv_color_hex(0x0F141F), 0);
    lv_obj_set_style_border_color(bottom_row, lv_color_hex(0x202B3D), 0);
    lv_obj_set_style_border_width(bottom_row, 1, 0);
    lv_obj_set_style_radius(bottom_row, 8, 0);
    lv_obj_set_style_pad_hor(bottom_row, 4, 0);
    lv_obj_set_style_pad_ver(bottom_row, 2, 0);
    lv_obj_clear_flag(bottom_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_spk = lv_label_create(bottom_row);
    lv_label_set_text(lbl_spk, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_spk, lv_color_hex(COLOR_ACCENT_AMBER), 0);
    lv_obj_set_style_text_font(lbl_spk, UI_FONT_12, 0);
    lv_obj_align(lbl_spk, LV_ALIGN_LEFT_MID, 2, 0);

    slider_volume = lv_slider_create(bottom_row);
    lv_obj_set_size(slider_volume, 68, 6);
    lv_obj_align(slider_volume, LV_ALIGN_LEFT_MID, 20, 0);
    lv_slider_set_range(slider_volume, 0, 100);
    lv_slider_set_value(slider_volume, music_player_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_volume, lv_color_hex(0x1F2937), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_volume, lv_color_hex(COLOR_ACCENT_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_volume, lv_color_hex(COLOR_TEXT_WHITE), LV_PART_KNOB);
    lv_obj_add_event_cb(slider_volume, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lbl_vol_val = lv_label_create(bottom_row);
    lv_label_set_text_fmt(lbl_vol_val, "%d%%", music_player_get_volume());
    lv_obj_set_style_text_color(lbl_vol_val, lv_color_hex(COLOR_ACCENT_AMBER), 0);
    lv_obj_set_style_text_font(lbl_vol_val, UI_FONT_12, 0);
    lv_obj_align(lbl_vol_val, LV_ALIGN_LEFT_MID, 94, 0);

    // Nút mở Playlist Drawer
    lv_obj_t *btn_playlist = lv_btn_create(bottom_row);
    lv_obj_set_size(btn_playlist, 42, 26);
    lv_obj_align(btn_playlist, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(btn_playlist, 6, 0);
    lv_obj_set_style_bg_color(btn_playlist, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_color(btn_playlist, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_width(btn_playlist, 1, 0);
    lv_obj_add_event_cb(btn_playlist, playlist_toggle_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_pl = lv_label_create(btn_playlist);
    lv_label_set_text(lbl_pl, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(lbl_pl, UI_FONT_12, 0);
    lv_obj_center(lbl_pl);

    // 6. MODAL PLAYLIST (Hiển thị khi người dùng cần chọn bài trong thẻ SD)
    playlist_modal = lv_obj_create(main_container);
    lv_obj_set_size(playlist_modal, SCREEN_WIDTH - 8, APP_CONTENT_HEIGHT - 6);
    lv_obj_center(playlist_modal);
    lv_obj_set_style_radius(playlist_modal, 14, 0);
    lv_obj_set_style_bg_color(playlist_modal, lv_color_hex(0x0C101A), 0);
    lv_obj_set_style_border_color(playlist_modal, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_border_width(playlist_modal, 1, 0);
    lv_obj_set_style_pad_all(playlist_modal, 6, 0);
    lv_obj_clear_flag(playlist_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(playlist_modal, LV_OBJ_FLAG_HIDDEN); // Mặc định ẩn

    // Header modal
    lv_obj_t *pl_header = lv_obj_create(playlist_modal);
    lv_obj_set_size(pl_header, SCREEN_WIDTH - 24, 28);
    lv_obj_align(pl_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(pl_header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pl_header, 0, 0);
    lv_obj_set_style_pad_all(pl_header, 0, 0);
    lv_obj_clear_flag(pl_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pl_title = lv_label_create(pl_header);
    lv_label_set_text(pl_title, LV_SYMBOL_DIRECTORY " Danh Sách /music");
    lv_obj_set_style_text_color(pl_title, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_text_font(pl_title, UI_FONT_TITLE, 0);
    lv_obj_align(pl_title, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *btn_close_pl = lv_btn_create(pl_header);
    lv_obj_set_size(btn_close_pl, 32, 26);
    lv_obj_align(btn_close_pl, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(btn_close_pl, 6, 0);
    lv_obj_set_style_bg_color(btn_close_pl, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_add_event_cb(btn_close_pl, playlist_toggle_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_x = lv_label_create(btn_close_pl);
    lv_label_set_text(lbl_x, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lbl_x, UI_FONT_12, 0);
    lv_obj_center(lbl_x);

    // Danh sách cuộn toàn chiều rộng
    music_list = lv_list_create(playlist_modal);
    lv_obj_set_size(music_list, SCREEN_WIDTH - 24, APP_CONTENT_HEIGHT - 44);
    lv_obj_align(music_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(music_list, lv_color_hex(0x121724), 0);
    lv_obj_set_style_border_width(music_list, 0, 0);
    lv_obj_set_style_radius(music_list, 8, 0);
    lv_obj_set_style_pad_all(music_list, 2, 0);

    int count = music_player_get_track_count();
    for (int i = 0; i < count; i++)
    {
        const MusicTrack *track = music_player_get_track(i);
        if (!track) continue;

        lv_obj_t *btn = lv_list_add_btn(music_list, LV_SYMBOL_AUDIO, track->title);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x182030), 0);
        lv_obj_set_style_pad_ver(btn, 8, 0);
        lv_obj_set_style_pad_hor(btn, 8, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_set_style_text_font(btn, UI_FONT_12, 0);
        lv_obj_add_event_cb(btn, track_item_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

/* Đóng và giải phóng tài nguyên ứng dụng Music Player */
void music_app_close(void)
{
    if (vinyl_anim_running)
    {
        lv_anim_del(vinyl_disc, anim_vinyl_rotate_cb);
        vinyl_anim_running = false;
    }
    main_container = nullptr;
    player_card = nullptr;
    playlist_modal = nullptr;
    music_list = nullptr;
    vinyl_disc = nullptr;
    lbl_track_title = nullptr;
    lbl_track_meta = nullptr;
    slider_progress = nullptr;
    lbl_cur_time = nullptr;
    lbl_total_time = nullptr;
    btn_play = nullptr;
    lbl_play_icon = nullptr;
    slider_volume = nullptr;
    lbl_vol_val = nullptr;
}

/* Cập nhật định kỳ giao diện */
void music_app_update(void)
{
    if (!main_container) return;

    bool is_playing = music_player_is_playing();

    // 1. Quản lý trạng thái Animation xoay đĩa than
    if (is_playing)
    {
        if (!vinyl_anim_running && vinyl_disc)
        {
            lv_anim_start(&vinyl_anim);
            vinyl_anim_running = true;
        }
    }
    else
    {
        if (vinyl_anim_running && vinyl_disc)
        {
            lv_anim_del(vinyl_disc, anim_vinyl_rotate_cb);
            vinyl_anim_running = false;
        }
    }

    // 2. Cập nhật icon nút Play / Pause
    if (lbl_play_icon)
    {
        lv_label_set_text(lbl_play_icon, is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    // 3. Cập nhật tiêu đề bài hát hiện tại
    int cur_idx = music_player_get_current_index();
    const MusicTrack *track = music_player_get_track(cur_idx);
    if (lbl_track_title && track)
    {
        lv_label_set_text(lbl_track_title, track->title);
    }

    // 4. Cập nhật thanh trượt tiến trình và thời gian
    uint32_t cur_sec = music_player_get_current_time();
    uint32_t dur_sec = music_player_get_duration();

    if (slider_progress && !is_user_dragging_slider)
    {
        lv_slider_set_range(slider_progress, 0, dur_sec > 0 ? dur_sec : 100);
        lv_slider_set_value(slider_progress, cur_sec, LV_ANIM_OFF);
    }

    if (lbl_cur_time)
    {
        char buf[16];
        music_player_format_time(cur_sec, buf, sizeof(buf));
        lv_label_set_text(lbl_cur_time, buf);
    }

    if (lbl_total_time)
    {
        char buf[16];
        music_player_format_time(dur_sec, buf, sizeof(buf));
        lv_label_set_text(lbl_total_time, buf);
    }
}
