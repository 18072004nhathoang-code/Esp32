/**
 * @file audio_app.cpp
 * @brief Giao diện ứng dụng Voice AI & Audio Lab cho màn hình Portrait 240x320
 * Bố cục: Cuộn dọc các card chuyên biệt: Mic Oscilloscope, Speaker & Soundboard, PSRAM Voice Memo
 */

#include "audio_app.h"
#include "../audio/audio_manager.h"
#include "../ui/ui_theme.h"
#include <stdio.h>

// Quản lý các thành phần giao diện
static lv_obj_t *main_container = nullptr;
static lv_obj_t *bar_vu_meter = nullptr;
static lv_obj_t *lbl_vu_val = nullptr;
static lv_obj_t *chart_waveform = nullptr;
static lv_chart_series_t *ser_waveform = nullptr;

static lv_obj_t *slider_vol = nullptr;
static lv_obj_t *lbl_vol_val = nullptr;

static lv_obj_t *btn_record = nullptr;
static lv_obj_t *lbl_record_btn = nullptr;
static lv_obj_t *btn_play = nullptr;
static lv_obj_t *lbl_play_btn = nullptr;
static lv_obj_t *lbl_recorder_status = nullptr;
static lv_obj_t *bar_record_progress = nullptr;

static bool is_app_active = false;

/* =========================================================================
 * CÁC CALLBACK SỰ KIỆN NÚT BẤM & SLIDER
 * ========================================================================= */
static void volume_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    audio_set_volume((uint8_t)val);
    if (lbl_vol_val)
    {
        lv_label_set_text_fmt(lbl_vol_val, "🔊 Loa: %d%%", val);
    }
}

static void sound_effect_btn_cb(lv_event_t *e)
{
    uintptr_t fx_id = (uintptr_t)lv_event_get_user_data(e);
    audio_play_sound_effect((SoundEffect)fx_id);
}

static void record_btn_cb(lv_event_t *e)
{
    if (audio_is_recording())
    {
        audio_stop_recording();
        lv_label_set_text(lbl_record_btn, LV_SYMBOL_PLAY " Thu");
        lv_obj_set_style_bg_color(btn_record, lv_color_hex(COLOR_ACCENT_RED), 0);
        if (lbl_recorder_status)
        {
            uint32_t dur = audio_get_recorded_duration_ms();
            lv_label_set_text_fmt(lbl_recorder_status, "✅ Đã lưu PSRAM: %.1fs", (float)dur / 1000.0f);
            lv_obj_set_style_text_color(lbl_recorder_status, lv_color_hex(COLOR_ACCENT_GREEN), 0);
        }
    }
    else
    {
        if (audio_start_recording(10))
        {
            lv_label_set_text(lbl_record_btn, LV_SYMBOL_STOP " Dừng");
            lv_obj_set_style_bg_color(btn_record, lv_color_hex(0xE53E3E), 0);
            if (lbl_recorder_status)
            {
                lv_label_set_text(lbl_recorder_status, "🔴 Đang thu âm từ Mic...");
                lv_obj_set_style_text_color(lbl_recorder_status, lv_color_hex(COLOR_ACCENT_RED), 0);
            }
        }
        else
        {
            if (lbl_recorder_status)
            {
                lv_label_set_text(lbl_recorder_status, "⚠️ Lỗi: Bus bận hoặc thiếu PSRAM!");
                lv_obj_set_style_text_color(lbl_recorder_status, lv_color_hex(COLOR_ACCENT_AMBER), 0);
            }
        }
    }
}

static void play_btn_cb(lv_event_t *e)
{
    if (audio_is_playing())
    {
        audio_stop_playback();
        lv_label_set_text(lbl_play_btn, LV_SYMBOL_AUDIO " Phát");
        lv_obj_set_style_bg_color(btn_play, lv_color_hex(COLOR_ACCENT_GREEN), 0);
        if (lbl_recorder_status)
        {
            lv_label_set_text(lbl_recorder_status, "⏹ Đã dừng phát lại");
            lv_obj_set_style_text_color(lbl_recorder_status, lv_color_hex(COLOR_TEXT_MUTED), 0);
        }
    }
    else
    {
        if (audio_start_playback())
        {
            lv_label_set_text(lbl_play_btn, LV_SYMBOL_STOP " Dừng");
            lv_obj_set_style_bg_color(btn_play, lv_color_hex(0x2B6CB0), 0);
            if (lbl_recorder_status)
            {
                lv_label_set_text(lbl_recorder_status, "🔊 Đang phát ra Loa...");
                lv_obj_set_style_text_color(lbl_recorder_status, lv_color_hex(COLOR_ACCENT_CYAN), 0);
            }
        }
        else
        {
            if (lbl_recorder_status)
            {
                lv_label_set_text(lbl_recorder_status, "⚠️ Chưa có bản ghi âm!");
                lv_obj_set_style_text_color(lbl_recorder_status, lv_color_hex(COLOR_ACCENT_AMBER), 0);
            }
        }
    }
}

/* =========================================================================
 * KHỞI TẠO GIAO DIỆN AUDIO LAB 240x320 PORTRAIT
 * ========================================================================= */
void audio_app_open(lv_obj_t *parent)
{
    if (!parent) return;

    main_container = parent;
    is_app_active = true;

    lv_obj_set_style_pad_all(parent, 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(COLOR_OS_BG), 0);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // =========================================================================
    // 1. CARD 1 (W: 226, H: 140): MICROPHONE LIVE OSCILLOSCOPE & VU METER
    // =========================================================================
    lv_obj_t *card_mic = lv_obj_create(parent);
    lv_obj_set_size(card_mic, SCREEN_WIDTH - 14, 140);
    lv_obj_align(card_mic, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(card_mic, 10, 0);
    lv_obj_set_style_bg_color(card_mic, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card_mic, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_border_width(card_mic, 1, 0);
    lv_obj_set_style_pad_all(card_mic, 6, 0);
    lv_obj_clear_flag(card_mic, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t_mic = lv_label_create(card_mic);
    lv_label_set_text(t_mic, "🎙️ Mic Live Waveform");
    lv_obj_set_style_text_color(t_mic, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_set_style_text_font(t_mic, &lv_font_montserrat_10, 0);
    lv_obj_align(t_mic, LV_ALIGN_TOP_LEFT, 0, 0);

    bar_vu_meter = lv_bar_create(card_mic);
    lv_obj_set_size(bar_vu_meter, SCREEN_WIDTH - 30, 8);
    lv_obj_align(bar_vu_meter, LV_ALIGN_TOP_MID, 0, 16);
    lv_bar_set_range(bar_vu_meter, 0, 100);
    lv_bar_set_value(bar_vu_meter, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_vu_meter, lv_color_hex(0x232D3F), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_vu_meter, lv_color_hex(COLOR_ACCENT_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_vu_meter, 3, 0);

    lbl_vu_val = lv_label_create(card_mic);
    lv_label_set_text(lbl_vu_val, "Mức thu: 0% | -60.0 dB");
    lv_obj_set_style_text_color(lbl_vu_val, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_vu_val, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_vu_val, LV_ALIGN_TOP_LEFT, 0, 28);

    chart_waveform = lv_chart_create(card_mic);
    lv_obj_set_size(chart_waveform, SCREEN_WIDTH - 30, 72);
    lv_obj_align(chart_waveform, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_chart_set_type(chart_waveform, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart_waveform, 32);
    lv_chart_set_range(chart_waveform, LV_CHART_AXIS_PRIMARY_Y, -3000, 3000);
    lv_chart_set_div_line_count(chart_waveform, 3, 4);
    lv_obj_set_style_bg_color(chart_waveform, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_border_color(chart_waveform, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(chart_waveform, 1, 0);
    lv_obj_set_style_radius(chart_waveform, 6, 0);

    ser_waveform = lv_chart_add_series(chart_waveform, lv_color_hex(COLOR_ACCENT_CYAN), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < 32; i++)
    {
        lv_chart_set_next_value(chart_waveform, ser_waveform, 0);
    }

    // =========================================================================
    // 2. CARD 2 (W: 226, H: 120): SPEAKER & SOUNDBOARD
    // =========================================================================
    lv_obj_t *card_spk = lv_obj_create(parent);
    lv_obj_set_size(card_spk, SCREEN_WIDTH - 14, 120);
    lv_obj_align(card_spk, LV_ALIGN_TOP_MID, 0, 146);
    lv_obj_set_style_radius(card_spk, 10, 0);
    lv_obj_set_style_bg_color(card_spk, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(card_spk, lv_color_hex(COLOR_ACCENT_AMBER), 0);
    lv_obj_set_style_border_width(card_spk, 1, 0);
    lv_obj_set_style_pad_all(card_spk, 6, 0);
    lv_obj_clear_flag(card_spk, LV_OBJ_FLAG_SCROLLABLE);

    lbl_vol_val = lv_label_create(card_spk);
    lv_label_set_text_fmt(lbl_vol_val, "🔊 Loa: %d%%", audio_get_volume());
    lv_obj_set_style_text_color(lbl_vol_val, lv_color_hex(COLOR_ACCENT_AMBER), 0);
    lv_obj_set_style_text_font(lbl_vol_val, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_vol_val, LV_ALIGN_TOP_LEFT, 0, 0);

    slider_vol = lv_slider_create(card_spk);
    lv_obj_set_size(slider_vol, SCREEN_WIDTH - 30, 8);
    lv_obj_align(slider_vol, LV_ALIGN_TOP_MID, 0, 16);
    lv_slider_set_range(slider_vol, 0, 100);
    lv_slider_set_value(slider_vol, audio_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_vol, lv_color_hex(COLOR_ACCENT_AMBER), LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider_vol, volume_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // 4 Nút Soundboard
    auto create_snd_btn = [&](const char *txt, int x, int y, uint32_t hex, SoundEffect fx) {
        lv_obj_t *b = lv_btn_create(card_spk);
        lv_obj_set_size(b, 100, 26);
        lv_obj_set_pos(b, x, y);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(hex), 0);
        lv_obj_add_event_cb(b, sound_effect_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)fx);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
        lv_obj_center(l);
    };

    create_snd_btn(LV_SYMBOL_BELL " Chime",  2,   34, 0x1E3A8A, FX_CHIME);
    create_snd_btn(LV_SYMBOL_CHARGE " Beep", 108, 34, 0x7C2D12, FX_BEEP);
    create_snd_btn(LV_SYMBOL_AUDIO " Melody",2,   66, 0x14532D, FX_MELODY);
    create_snd_btn("🤖 XiaoZhi",              108, 66, 0x581C87, FX_XIAOZHI_WAKE);

    // =========================================================================
    // 3. CARD 3 (W: 226, H: 96): PSRAM VOICE MEMO
    // =========================================================================
    lv_obj_t *memo_box = lv_obj_create(parent);
    lv_obj_set_size(memo_box, SCREEN_WIDTH - 14, 96);
    lv_obj_align(memo_box, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_set_style_radius(memo_box, 10, 0);
    lv_obj_set_style_bg_color(memo_box, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(memo_box, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_border_width(memo_box, 1, 0);
    lv_obj_set_style_pad_all(memo_box, 6, 0);
    lv_obj_clear_flag(memo_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t_memo = lv_label_create(memo_box);
    lv_label_set_text(t_memo, "🎙️ Ghi Âm PSRAM (Tối đa 10s)");
    lv_obj_set_style_text_color(t_memo, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_text_font(t_memo, &lv_font_montserrat_10, 0);
    lv_obj_align(t_memo, LV_ALIGN_TOP_LEFT, 0, 0);

    btn_record = lv_btn_create(memo_box);
    lv_obj_set_size(btn_record, 98, 28);
    lv_obj_set_pos(btn_record, 2, 18);
    lv_obj_set_style_radius(btn_record, 6, 0);
    lv_obj_set_style_bg_color(btn_record, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_add_event_cb(btn_record, record_btn_cb, LV_EVENT_CLICKED, nullptr);

    lbl_record_btn = lv_label_create(btn_record);
    lv_label_set_text(lbl_record_btn, LV_SYMBOL_PLAY " Thu");
    lv_obj_set_style_text_font(lbl_record_btn, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl_record_btn);

    btn_play = lv_btn_create(memo_box);
    lv_obj_set_size(btn_play, 98, 28);
    lv_obj_set_pos(btn_play, 108, 18);
    lv_obj_set_style_radius(btn_play, 6, 0);
    lv_obj_set_style_bg_color(btn_play, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_add_event_cb(btn_play, play_btn_cb, LV_EVENT_CLICKED, nullptr);

    lbl_play_btn = lv_label_create(btn_play);
    lv_label_set_text(lbl_play_btn, LV_SYMBOL_AUDIO " Phát");
    lv_obj_set_style_text_font(lbl_play_btn, &lv_font_montserrat_10, 0);
    lv_obj_center(lbl_play_btn);

    lbl_recorder_status = lv_label_create(memo_box);
    lv_label_set_text(lbl_recorder_status, "Sẵn sàng ghi âm");
    lv_obj_set_style_text_color(lbl_recorder_status, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_recorder_status, &lv_font_montserrat_10, 0);
    lv_obj_align(lbl_recorder_status, LV_ALIGN_BOTTOM_LEFT, 2, -10);

    bar_record_progress = lv_bar_create(memo_box);
    lv_obj_set_size(bar_record_progress, SCREEN_WIDTH - 30, 4);
    lv_obj_align(bar_record_progress, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(bar_record_progress, 0, 100);
    lv_bar_set_value(bar_record_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_record_progress, lv_color_hex(0x232D3F), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_record_progress, lv_color_hex(COLOR_ACCENT_CYAN), LV_PART_INDICATOR);
}

void audio_app_close(void)
{
    is_app_active = false;
    main_container = nullptr;
    bar_vu_meter = nullptr;
    lbl_vu_val = nullptr;
    chart_waveform = nullptr;
    ser_waveform = nullptr;
    slider_vol = nullptr;
    lbl_vol_val = nullptr;
    btn_record = nullptr;
    lbl_record_btn = nullptr;
    btn_play = nullptr;
    lbl_play_btn = nullptr;
    lbl_recorder_status = nullptr;
    bar_record_progress = nullptr;
}

/* =========================================================================
 * CẬP NHẬT ĐỊNH KỲ (WAVEFORM & VU METER TELEMETRY)
 * ========================================================================= */
void audio_app_update(void)
{
    if (!is_app_active || !chart_waveform || !ser_waveform) return;

    uint8_t mic_level = audio_get_mic_level();
    float mic_db = audio_get_mic_db();

    if (bar_vu_meter)
    {
        lv_bar_set_value(bar_vu_meter, mic_level, LV_ANIM_OFF);
        if (mic_level > 80)
            lv_obj_set_style_bg_color(bar_vu_meter, lv_color_hex(COLOR_ACCENT_RED), LV_PART_INDICATOR);
        else if (mic_level > 40)
            lv_obj_set_style_bg_color(bar_vu_meter, lv_color_hex(COLOR_ACCENT_AMBER), LV_PART_INDICATOR);
        else
            lv_obj_set_style_bg_color(bar_vu_meter, lv_color_hex(COLOR_ACCENT_GREEN), LV_PART_INDICATOR);
    }

    if (lbl_vu_val)
    {
        const char *state_txt = "Im lặng";
        if (mic_level > 60) state_txt = "Cao!";
        else if (mic_level > 20) state_txt = "Đang nói...";
        lv_label_set_text_fmt(lbl_vu_val, "%d%% | %.1fdB (%s)", mic_level, mic_db, state_txt);
    }

    int16_t wave_samples[32];
    audio_get_waveform_samples(wave_samples, 32);
    for (int i = 0; i < 32; i++)
    {
        lv_chart_set_next_value(chart_waveform, ser_waveform, wave_samples[i]);
    }

    if (bar_record_progress)
    {
        if (audio_is_recording())
        {
            uint32_t dur = audio_get_recorded_duration_ms();
            int progress = (dur * 100) / (AUDIO_RECORD_MAX_SEC * 1000);
            if (progress > 100) progress = 100;
            lv_bar_set_value(bar_record_progress, progress, LV_ANIM_OFF);
            if (lbl_recorder_status)
            {
                lv_label_set_text_fmt(lbl_recorder_status, "🔴 Thu: %.1fs / %ds",
                                      (float)dur / 1000.0f, AUDIO_RECORD_MAX_SEC);
            }
        }
        else if (audio_is_playing())
        {
            uint32_t prog = audio_get_playback_progress_ms();
            uint32_t total = audio_get_recorded_duration_ms();
            int progress = (total > 0) ? ((prog * 100) / total) : 0;
            if (progress > 100) progress = 100;
            lv_bar_set_value(bar_record_progress, progress, LV_ANIM_OFF);
            if (lbl_recorder_status)
            {
                lv_label_set_text_fmt(lbl_recorder_status, "🟢 Phát: %.1fs / %.1fs",
                                      (float)prog / 1000.0f, (float)total / 1000.0f);
            }
        }
    }
}
