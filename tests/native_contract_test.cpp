#include "firmware_contracts.h"
#include "touch_contact_tracker.h"
#include "touch_transform.h"
#include <assert.h>
#include <string.h>

int main()
{
    TouchTransformConfig transform = {240, 320, 240, 320, 2, false, true, true};
    uint16_t x = 0, y = 0;
    assert(touch_transform_point(transform, 0, 0, &x, &y) && x == 0 && y == 0);
    assert(touch_transform_point(transform, 239, 319, &x, &y) && x == 239 && y == 319);
    assert(!touch_transform_point(transform, 240, 0, &x, &y));

    uint8_t ids[] = {3, 8};
    uint8_t events[] = {2, 2};
    TouchContactDecision selected = touch_contact_select(8, ids, events, 2);
    assert(selected.index == 1 && !selected.release_before_switch);
    selected = touch_contact_select(7, ids, events, 2);
    assert(selected.index == -1 && selected.release_before_switch);

    int16_t local_x = 0, local_y = 0;
    assert(ui_screen_to_local(100, 130, 0, 30, 240, 290, &local_x, &local_y));
    assert(local_x == 100 && local_y == 100);
    assert(!ui_screen_to_local(20, 29, 0, 30, 240, 290, &local_x, &local_y));

    assert(exclusive_start_can_claim(0, 0));
    assert(!exclusive_start_can_claim(1, 0));
    assert(audio_stereo_frames_from_bytes(1024) == 256);
    assert(audio_stereo_frames_from_bytes(1023) == 255);
    assert(audio_rx_carry_after_bytes(1023) == 3);
    assert(audio_write_completed(1024, 1024));
    assert(!audio_write_completed(1020, 1024));
    assert(es8311_volume_register(0) == 0);
    assert(es8311_volume_register(100) == 0xBF);
    assert(es8311_volume_register(50) < 0xBF);
    assert(audio_session_cleanup_allowed(7, 7));
    assert(!audio_session_cleanup_allowed(6, 7));

    assert(camera_session_accepts(12, 12, true));
    assert(!camera_session_accepts(11, 12, true));
    assert(camera_control_needs_apply(5, 4));
    assert(!camera_control_needs_apply(5, 5));
    assert(camera_control_can_ack(true, true, true, false));
    assert(!camera_control_can_ack(false, true, true, false));
    assert(!camera_control_can_ack(true, false, true, false));

    assert(wifi_generation_can_commit(8, 8, 8, false));
    assert(!wifi_generation_can_commit(8, 9, 8, false));
    assert(!wifi_generation_can_commit(8, 8, 8, true));
    assert(estimated_cpu_usage_from_rates(80, 100) == 20);
    assert(estimated_cpu_usage_from_rates(160, 200) == 20);

    const uint8_t jpeg[] = {0xFF, 0xD8, 0x11, 0xFF, 0xD9};
    assert(complete_jpeg_signature(jpeg, sizeof(jpeg)));
    assert(!complete_jpeg_signature(jpeg, sizeof(jpeg) - 1));
    assert(cache_temp_can_replace(true, true, true));
    assert(!cache_temp_can_replace(false, true, true));

    assert(request_response_is_current(4, 4, 3));
    assert(!request_response_is_current(4, 5, 3));
    assert(!request_response_is_current(4, 4, 4));
    assert(bounded_body_append_allowed(16380, 4, 16384));
    assert(!bounded_body_append_allowed(16380, 5, 16384));
    assert(http_dechunked_body_complete(-1, 7, 7, false));
    assert(!http_dechunked_body_complete(8, 7, 7, false));
    assert(!http_dechunked_body_complete(-1, 7, -1, false));
    const char vietnamese[] = "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t";
    assert(valid_utf8_text(vietnamese, sizeof(vietnamese) - 1));
    const char invalid_utf8[] = {char(0xC0), char(0xAF)};
    assert(!valid_utf8_text(invalid_utf8, sizeof(invalid_utf8)));

    uint8_t wav[48] = {};
    memcpy(wav, "RIFF", 4); wav[4] = 40;
    memcpy(wav + 8, "WAVEfmt ", 8); wav[16] = 16;
    wav[20] = 1; wav[22] = 1; wav[24] = 0x80; wav[25] = 0x3E;
    wav[28] = 0x00; wav[29] = 0x7D; wav[32] = 2; wav[34] = 16;
    memcpy(wav + 36, "data", 4); wav[40] = 4;
    PcmWavView view = {};
    assert(parse_pcm16_mono_16k_wav(wav, sizeof(wav), &view) && view.sample_count == 2);
    assert(!parse_pcm16_mono_16k_wav(wav, sizeof(wav) - 1, &view));
    wav[22] = 2;
    assert(!parse_pcm16_mono_16k_wav(wav, sizeof(wav), &view));
    return 0;
}
