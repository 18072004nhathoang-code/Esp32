/**
 * @file music_app.cpp
 * @brief Triển khai giao diện ứng dụng Music Player Pro Max trên LVGL 8 (480x266)
 * Chia 2 cột: Nửa trái danh sách MP3 từ /music, Nửa phải đĩa than quay và điều khiển cảm ứng
 */

#include "music_app.h"
#include "../audio/music_player.h"

// Các thành phần widget giao diện
static lv_obj_t *main_container = nullptr;
static lv_obj_t *music_list = nullptr;
static lv_obj_t *lbl_header_count = nullptr;
static lv_obj_t *right_panel = nullptr;
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

/* Callback khi bấm vào bài hát trong danh sách bên trái */
static void track_item_click_cb(lv_event_t *e)
{
    uintptr_t track_idx = (uintptr_t)lv_event_get_user_data(e);
    music_player_play_index((int)track_idx);
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

/* Mở ứng dụng Music Player */
void music_app_open(lv_obj_t *parent)
{
    if (!parent) return;

    main_container = parent;
    lv_obj_set_style_pad_all(main_container, 4, 0);
    lv_obj_clear_flag(main_container, LV_OBJ_FLAG_SCROLLABLE);

    // =========================================================================
    // 1. CỘT TRÁI (200px): DANH SÁCH CUỘN BÀI HÁT TỪ THƯ MỤC /music TRÊN THẺ NHỚ
    // =========================================================================
    lv_obj_t *left_panel = lv_obj_create(main_container);
    lv_obj_set_size(left_panel, 196, 258);
    lv_obj_set_pos(left_panel, 0, 0);
    lv_obj_set_style_radius(left_panel, 12, 0);
    lv_obj_set_style_bg_color(left_panel, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(left_panel, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_border_width(left_panel, 1, 0);
    lv_obj_set_style_pad_all(left_panel, 6, 0);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Tiêu đề danh sách: 📁 Thư Viện (/music)
    lv_obj_t *left_header = lv_obj_create(left_panel);
    lv_obj_set_size(left_header, 184, 26);
    lv_obj_align(left_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(left_header, lv_color_hex(0x121824), 0);
    lv_obj_set_style_border_width(left_header, 0, 0);
    lv_obj_set_style_radius(left_header, 6, 0);
    lv_obj_set_style_pad_hor(left_header, 6, 0);
    lv_obj_set_style_pad_ver(left_header, 3, 0);
    lv_obj_clear_flag(left_header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_hdr = lv_label_create(left_header);
    lv_label_set_text(lbl_hdr, LV_SYMBOL_DIRECTORY " /music");
    lv_obj_set_style_text_color(lbl_hdr, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_hdr, LV_ALIGN_LEFT_MID, 0, 0);

    lbl_header_count = lv_label_create(left_header);
    lv_label_set_text_fmt(lbl_header_count, "(%d bài)", music_player_get_track_count());
    lv_obj_set_style_text_color(lbl_header_count, lv_color_hex(0x718096), 0);
    lv_obj_set_style_text_font(lbl_header_count, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_header_count, LV_ALIGN_RIGHT_MID, 0, 0);

    // Danh sách cuộn lv_list
    music_list = lv_list_create(left_panel);
    lv_obj_set_size(music_list, 184, 214);
    lv_obj_align(music_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(music_list, lv_color_hex(0x0E131E), 0);
    lv_obj_set_style_border_width(music_list, 0, 0);
    lv_obj_set_style_radius(music_list, 8, 0);
    lv_obj_set_style_pad_all(music_list, 4, 0);

    int count = music_player_get_track_count();
    for (int i = 0; i < count; i++)
    {
        const MusicTrack *track = music_player_get_track(i);
        if (!track) continue;

        lv_obj_t *btn = lv_list_add_btn(music_list, LV_SYMBOL_AUDIO, track->title);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x161B26), 0);
        lv_obj_set_style_pad_ver(btn, 6, 0);
        lv_obj_set_style_pad_hor(btn, 6, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xE2E8F0), 0);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_12, 0);
        lv_obj_add_event_cb(btn, track_item_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    // =========================================================================
    // 2. CỘT PHẢI (274px): GIAO DIỆN PHÁT NHẠC, ĐĨA THAN QUAY & ĐIỀU KHIỂN CẢM ỨNG
    // =========================================================================
    right_panel = lv_obj_create(main_container);
    lv_obj_set_size(right_panel, 274, 258);
    lv_obj_set_pos(right_panel, 202, 0);
    lv_obj_set_style_radius(right_panel, 12, 0);
    lv_obj_set_style_bg_color(right_panel, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_border_color(right_panel, lv_color_hex(0x9D4EDD), 0);
    lv_obj_set_style_border_width(right_panel, 1, 0);
    lv_obj_set_style_pad_all(right_panel, 6, 0);
    lv_obj_clear_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);

    // 2.1 ĐĨA THAN QUAY TRÒN (VINYL DISC) VÀ THÔNG TIN BÀI HÁT
    lv_obj_t *top_meta_box = lv_obj_create(right_panel);
    lv_obj_set_size(top_meta_box, 262, 88);
    lv_obj_align(top_meta_box, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top_meta_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_meta_box, 0, 0);
    lv_obj_set_style_pad_all(top_meta_box, 0, 0);
    lv_obj_clear_flag(top_meta_box, LV_OBJ_FLAG_SCROLLABLE);

    // Đĩa than Vinyl (Vòng tròn ngoài màu đen obsidian)
    vinyl_disc = lv_obj_create(top_meta_box);
    lv_obj_set_size(vinyl_disc, 76, 76);
    lv_obj_align(vinyl_disc, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_radius(vinyl_disc, 38, 0);
    lv_obj_set_style_bg_color(vinyl_disc, lv_color_hex(0x0C0E14), 0);
    lv_obj_set_style_border_color(vinyl_disc, lv_color_hex(0x2A3346), 0);
    lv_obj_set_style_border_width(vinyl_disc, 3, 0);
    lv_obj_set_style_shadow_width(vinyl_disc, 8, 0);
    lv_obj_set_style_shadow_color(vinyl_disc, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(vinyl_disc, LV_OBJ_FLAG_SCROLLABLE);

    // Vòng rãnh đĩa than (Groove Ring)
    lv_obj_t *groove = lv_obj_create(vinyl_disc);
    lv_obj_set_size(groove, 54, 54);
    lv_obj_center(groove);
    lv_obj_set_style_radius(groove, 27, 0);
    lv_obj_set_style_bg_opa(groove, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(groove, lv_color_hex(0x1F2837), 0);
    lv_obj_set_style_border_width(groove, 1, 0);
    lv_obj_clear_flag(groove, LV_OBJ_FLAG_SCROLLABLE);

    // Nhãn tâm đĩa than màu Neon nổi bật
    lv_obj_t *vinyl_label = lv_obj_create(vinyl_disc);
    lv_obj_set_size(vinyl_label, 32, 32);
    lv_obj_center(vinyl_label);
    lv_obj_set_style_radius(vinyl_label, 16, 0);
    lv_obj_set_style_bg_color(vinyl_label, lv_color_hex(0x9D4EDD), 0);
    lv_obj_set_style_border_color(vinyl_label, lv_color_hex(0x00F2FE), 0);
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

    // Thiết lập Animation xoay vòng đĩa than
    lv_anim_init(&vinyl_anim);
    lv_anim_set_var(&vinyl_anim, vinyl_disc);
    lv_anim_set_values(&vinyl_anim, 0, 3600); // 360.0 độ trong LVGL 8
    lv_anim_set_time(&vinyl_anim, 3000);     // 3 giây một vòng quay
    lv_anim_set_repeat_count(&vinyl_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&vinyl_anim, anim_vinyl_rotate_cb);
    vinyl_anim_running = false;

    // Tên bài hát (cuộn tự động nếu dài)
    lbl_track_title = lv_label_create(top_meta_box);
    lv_label_set_long_mode(lbl_track_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(lbl_track_title, 160);
    lv_obj_align(lbl_track_title, LV_ALIGN_TOP_LEFT, 92, 12);
    lv_obj_set_style_text_color(lbl_track_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_track_title, &lv_font_montserrat_14, 0);

    const MusicTrack *cur_track = music_player_get_track(music_player_get_current_index());
    if (cur_track)
    {
        lv_label_set_text(lbl_track_title, cur_track->title);
    }
    else
    {
        lv_label_set_text(lbl_track_title, "No Song Loaded");
    }

    // Phụ đề định dạng âm thanh
    lbl_track_meta = lv_label_create(top_meta_box);
    lv_label_set_text(lbl_track_meta, "MP3 • 320k • 44.1kHz • SD");
    lv_obj_align(lbl_track_meta, LV_ALIGN_TOP_LEFT, 92, 42);
    lv_obj_set_style_text_color(lbl_track_meta, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_text_font(lbl_track_meta, &lv_font_montserrat_12, 0);

    // 2.2 THANH TRƯỢT TIẾN TRÌNH (PROGRESS SLIDER) VÀ ĐỒNG HỒ PHÚT:GIÂY
    lv_obj_t *progress_box = lv_obj_create(right_panel);
    lv_obj_set_size(progress_box, 262, 44);
    lv_obj_align(progress_box, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_bg_opa(progress_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(progress_box, 0, 0);
    lv_obj_set_style_pad_all(progress_box, 0, 0);
    lv_obj_clear_flag(progress_box, LV_OBJ_FLAG_SCROLLABLE);

    slider_progress = lv_slider_create(progress_box);
    lv_obj_set_size(slider_progress, 256, 8);
    lv_obj_align(slider_progress, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(slider_progress, lv_color_hex(0x2D3748), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_progress, lv_color_hex(0x00F2FE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_progress, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_progress, 2, LV_PART_KNOB);
    lv_slider_set_range(slider_progress, 0, music_player_get_duration());
    lv_slider_set_value(slider_progress, 0, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_progress, progress_slider_event_cb, LV_EVENT_ALL, nullptr);

    lbl_cur_time = lv_label_create(progress_box);
    lv_label_set_text(lbl_cur_time, "00:00");
    lv_obj_set_style_text_color(lbl_cur_time, lv_color_hex(0xA0AEC0), 0);
    lv_obj_set_style_text_font(lbl_cur_time, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_cur_time, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    lbl_total_time = lv_label_create(progress_box);
    char total_buf[16];
    music_player_format_time(music_player_get_duration(), total_buf, sizeof(total_buf));
    lv_label_set_text(lbl_total_time, total_buf);
    lv_obj_set_style_text_color(lbl_total_time, lv_color_hex(0xA0AEC0), 0);
    lv_obj_set_style_text_font(lbl_total_time, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_total_time, LV_ALIGN_BOTTOM_RIGHT, -4, 0);

    // 2.3 CỤM NÚT ĐIỀU KHIỂN CẢM ỨNG (⏮ ▶/⏸ ⏭)
    lv_obj_t *ctrl_box = lv_obj_create(right_panel);
    lv_obj_set_size(ctrl_box, 262, 54);
    lv_obj_align(ctrl_box, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_bg_opa(ctrl_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_box, 0, 0);
    lv_obj_set_style_pad_all(ctrl_box, 0, 0);
    lv_obj_clear_flag(ctrl_box, LV_OBJ_FLAG_SCROLLABLE);

    // Nút Previous ⏮
    lv_obj_t *btn_prev = lv_btn_create(ctrl_box);
    lv_obj_set_size(btn_prev, 40, 40);
    lv_obj_align(btn_prev, LV_ALIGN_CENTER, -62, 0);
    lv_obj_set_style_radius(btn_prev, 20, 0);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_color(btn_prev, lv_color_hex(0x374151), 0);
    lv_obj_set_style_border_width(btn_prev, 1, 0);
    lv_obj_add_event_cb(btn_prev, prev_btn_click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(lbl_prev, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_prev);

    // Nút Play / Pause ▶ / ⏸ (Nổi bật nhất ở giữa)
    btn_play = lv_btn_create(ctrl_box);
    lv_obj_set_size(btn_play, 48, 48);
    lv_obj_align(btn_play, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(btn_play, 24, 0);
    lv_obj_set_style_bg_color(btn_play, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_shadow_width(btn_play, 12, 0);
    lv_obj_set_style_shadow_color(btn_play, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_shadow_opa(btn_play, LV_OPA_60, 0);
    lv_obj_add_event_cb(btn_play, play_btn_click_cb, LV_EVENT_CLICKED, nullptr);

    lbl_play_icon = lv_label_create(btn_play);
    lv_label_set_text(lbl_play_icon, music_player_is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(lbl_play_icon, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_text_font(lbl_play_icon, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_play_icon);

    // Nút Next ⏭
    lv_obj_t *btn_next = lv_btn_create(ctrl_box);
    lv_obj_set_size(btn_next, 40, 40);
    lv_obj_align(btn_next, LV_ALIGN_CENTER, 62, 0);
    lv_obj_set_style_radius(btn_next, 20, 0);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_color(btn_next, lv_color_hex(0x374151), 0);
    lv_obj_set_style_border_width(btn_next, 1, 0);
    lv_obj_add_event_cb(btn_next, next_btn_click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(lbl_next, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_next);

    // 2.4 THANH TRƯỢT ÂM LƯỢNG (VOLUME SLIDER)
    lv_obj_t *vol_box = lv_obj_create(right_panel);
    lv_obj_set_size(vol_box, 262, 42);
    lv_obj_align(vol_box, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(vol_box, lv_color_hex(0x121824), 0);
    lv_obj_set_style_border_width(vol_box, 0, 0);
    lv_obj_set_style_radius(vol_box, 8, 0);
    lv_obj_set_style_pad_hor(vol_box, 10, 0);
    lv_obj_set_style_pad_ver(vol_box, 4, 0);
    lv_obj_clear_flag(vol_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_vol_icon = lv_label_create(vol_box);
    lv_label_set_text(lbl_vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(lbl_vol_icon, lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_text_font(lbl_vol_icon, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_vol_icon, LV_ALIGN_LEFT_MID, 0, 0);

    slider_volume = lv_slider_create(vol_box);
    lv_obj_set_size(slider_volume, 160, 6);
    lv_obj_align(slider_volume, LV_ALIGN_CENTER, 4, 0);
    lv_obj_set_style_bg_color(slider_volume, lv_color_hex(0x2D3748), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_volume, lv_color_hex(0xFFB300), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_volume, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_volume, 1, LV_PART_KNOB);
    lv_slider_set_range(slider_volume, 0, 100);
    lv_slider_set_value(slider_volume, music_player_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_volume, volume_slider_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lbl_vol_val = lv_label_create(vol_box);
    lv_label_set_text_fmt(lbl_vol_val, "%d%%", music_player_get_volume());
    lv_obj_set_style_text_color(lbl_vol_val, lv_color_hex(0xFFB300), 0);
    lv_obj_set_style_text_font(lbl_vol_val, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_vol_val, LV_ALIGN_RIGHT_MID, 0, 0);
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
    music_list = nullptr;
    lbl_header_count = nullptr;
    right_panel = nullptr;
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
