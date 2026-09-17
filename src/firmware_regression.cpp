#include "firmware_regression.h"
#include "board_config.h"
#include "firmware_contracts.h"
#include "touch_contact_tracker.h"
#include "touch_transform.h"
#include "service_state_logic.h"
#include "ai/ai_voice_service.h"

static_assert(audio_stereo_frames_from_bytes(1024) == 256, "16 kHz DMA frame accounting");
static_assert(audio_stereo_frames_from_bytes(1023) == 255, "partial DMA reads must not overrun");
static_assert(audio_rx_carry_after_bytes(1023) == 3, "partial DMA bytes must carry to next read");
static_assert(audio_write_completed(1024, 1024), "complete I2S write accepted");
static_assert(!audio_write_completed(1020, 1024), "partial terminal I2S write rejected");
static_assert(bounded_body_append_allowed(16380, 4, 16384), "exact JSON limit accepted");
static_assert(!bounded_body_append_allowed(16380, 5, 16384), "oversize JSON rejected");
static_assert(http_dechunked_body_complete(-1, 128, 128, false), "chunked body accepted after dechunk");
static_assert(!http_dechunked_body_complete(128, 127, 127, false), "truncated body rejected");
static_assert(camera_session_accepts(7, 7, true), "current camera session accepted");
static_assert(!camera_session_accepts(6, 7, true), "stale camera session rejected");
static_assert(camera_control_needs_apply(9, 8), "critical camera control retries until ACK");
static_assert(!camera_control_needs_apply(9, 9), "camera control ACK terminates retry");
static_assert(request_response_is_current(3, 3, 2), "current AI response accepted");
static_assert(!request_response_is_current(3, 4, 2), "stale AI response rejected");
static_assert(!request_response_is_current(3, 3, 3), "cancelled AI response rejected");
static_assert(exclusive_start_can_claim(0, 0), "idle audio start may claim");
static_assert(!exclusive_start_can_claim(1, 0), "concurrent audio start rejected");
static_assert(es8311_volume_register(0) == 0, "zero volume is hardware mute");
static_assert(es8311_volume_register(100) == 0xBF, "100 percent stays at codec unity");
static_assert(wifi_generation_can_commit(5, 5, 5, false), "current WiFi save may commit");
static_assert(!wifi_generation_can_commit(5, 6, 5, false), "stale WiFi save rejected");
static_assert(estimated_cpu_usage_from_rates(80, 100) == 20, "CPU estimate normalized by time");
static_assert(estimated_cpu_usage_from_rates(160, 200) == 20, "CPU estimate independent of interval");
static_assert(camera_control_can_ack(true, true, true, false), "camera ACK after target reached");
static_assert(!camera_control_can_ack(false, true, true, false), "failed camera start not ACKed");

bool firmware_regression_run()
{
    bool ok = true;

    const TouchTransformConfig transform = {
        BOARD_LCD_PANEL_WIDTH, BOARD_LCD_PANEL_HEIGHT,
        BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, BOARD_LCD_ROTATION,
        BOARD_TOUCH_SWAP_XY != 0, BOARD_TOUCH_INVERT_X != 0, BOARD_TOUCH_INVERT_Y != 0
    };
    uint16_t sx = 0, sy = 0;
    ok = ok && touch_transform_point(transform, 0, 0, &sx, &sy) && sx == 0 && sy == 0;
    ok = ok && touch_transform_point(transform, 239, 319, &sx, &sy) && sx == 239 && sy == 319;

    const uint8_t ids_same[] = {4, 7};
    const uint8_t events_same[] = {2, 2};
    TouchContactDecision decision = touch_contact_select(7, ids_same, events_same, 2);
    ok = ok && decision.index == 1 && !decision.release_before_switch;
    const uint8_t ids_changed[] = {4};
    decision = touch_contact_select(7, ids_changed, events_same, 1);
    ok = ok && decision.index < 0 && decision.release_before_switch;

    int16_t lx = 0, ly = 0;
    ok = ok && ui_screen_to_local(120, 160, 0, 30, 240, 290, &lx, &ly) &&
         lx == 120 && ly == 130;
    ok = ok && !ui_screen_to_local(10, 29, 0, 30, 240, 290, &lx, &ly);

    uint8_t wav[48] = {};
    memcpy(wav, "RIFF", 4); wav[4] = 40;
    memcpy(wav + 8, "WAVEfmt ", 8); wav[16] = 16;
    wav[20] = 1; wav[22] = 1; wav[24] = 0x80; wav[25] = 0x3E;
    wav[28] = 0x00; wav[29] = 0x7D; wav[32] = 2; wav[34] = 16;
    memcpy(wav + 36, "data", 4); wav[40] = 4;
    PcmWavView view = {};
    ok = ok && parse_pcm16_mono_16k_wav(wav, sizeof(wav), &view) && view.sample_count == 2;
    ok = ok && !parse_pcm16_mono_16k_wav(wav, sizeof(wav) - 1, &view);
    wav[24] = 0x44;
    ok = ok && !parse_pcm16_mono_16k_wav(wav, sizeof(wav), &view);
    ok = ok && audio_control_applies_to_generation(10, 11);
    ok = ok && !audio_control_applies_to_generation(12, 11);
    ok = ok && !audio_pause_ack_is_current(7, 7, false);
    ok = ok && !transactional_remove_new_final(false, false);
    ok = ok && transactional_remove_new_final(true, false);
    ok = ok && transactional_keep_recovery_file(true, false);
    ok = ok && !wifi_connect_may_save(true, true, false);
    ok = ok && !camera_config_transaction_complete(true, true, true, false);
    ok = ok && camera_config_transaction_complete(true, true, true, true);
    ok = ok && !audio_music_handoff_can_grant(false, true);
    ok = ok && !audio_duplex_restore_ready(true, false);
    ok = ok && ai_voice_json_regression_test();
    return ok;
}
