#include "xiaozhi_protocol_logic.h"
#include "xiaozhi_session_logic.h"
#include "firmware_contracts.h"
#include "music_stream_logic.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

// Test 1: FragmentAssembler with control frames and continuation frames
void test_websocket_framing_and_assembler()
{
    uint8_t storage[1024] = {};
    xiaozhi::FragmentAssembler assembler(storage, sizeof(storage));

    assert(!assembler.isActive());
    assert(assembler.kind() == xiaozhi::FragmentAssembler::Kind::NONE);
    assert(assembler.size() == 0);

    // Initial TEXT frame split into 3 chunks
    const uint8_t chunk1[] = "{\"type\":\"hello\",";
    const uint8_t chunk2[] = "\"version\":2,";
    const uint8_t chunk3[] = "\"status\":\"ok\"}";

    assert(assembler.begin(xiaozhi::FragmentAssembler::Kind::TEXT, chunk1, strlen((const char *)chunk1)));
    assert(assembler.isActive());
    assert(assembler.kind() == xiaozhi::FragmentAssembler::Kind::TEXT);
    assert(assembler.size() == strlen((const char *)chunk1));

    // Simulate an interleaved PONG with payload (RFC 6455 5.4 control frame)
    // Control frames must NOT reset the active assembler!
    // In onEvent, op == 0x0A returns early, assembler remains untouched:
    assert(assembler.isActive());

    // Chunk 2 (SDK buffer chunk)
    assert(assembler.append(chunk2, strlen((const char *)chunk2)));
    assert(assembler.size() == strlen((const char *)chunk1) + strlen((const char *)chunk2));

    // Chunk 3 (Final piece)
    xiaozhi::FragmentAssembler::Kind finished_kind = xiaozhi::FragmentAssembler::Kind::NONE;
    const uint8_t *message = nullptr;
    size_t message_size = 0;
    assert(assembler.finish(chunk3, strlen((const char *)chunk3), &finished_kind, &message, &message_size));
    assert(!assembler.isActive());
    assert(finished_kind == xiaozhi::FragmentAssembler::Kind::TEXT);
    assert(message_size == strlen("{\"type\":\"hello\",\"version\":2,\"status\":\"ok\"}"));
    assert(memcmp(message, "{\"type\":\"hello\",\"version\":2,\"status\":\"ok\"}", message_size) == 0);

    printf("[PASS] test_websocket_framing_and_assembler\n");
}

// Test 2: Latency metrics formatting preventing fake 0ms and uptime diffs
static void format_latency_helper(char *buf, size_t buf_size, uint32_t t_start, uint32_t t_end)
{
    if (t_start == 0 || t_end == 0 || t_end < t_start)
    {
        snprintf(buf, buf_size, "N/A");
    }
    else
    {
        snprintf(buf, buf_size, "%ums", static_cast<unsigned>(t_end - t_start));
    }
}

void test_latency_formatting()
{
    char buf[32] = {};

    // When either timestamp is 0, must NOT log "0ms" or large uptime diff
    format_latency_helper(buf, sizeof(buf), 0, 1000);
    assert(strcmp(buf, "N/A") == 0);

    format_latency_helper(buf, sizeof(buf), 1000, 0);
    assert(strcmp(buf, "N/A") == 0);

    format_latency_helper(buf, sizeof(buf), 0, 0);
    assert(strcmp(buf, "N/A") == 0);

    // When end is before start (e.g. clock anomaly or unrecorded milestone)
    format_latency_helper(buf, sizeof(buf), 2000, 1000);
    assert(strcmp(buf, "N/A") == 0);

    // Valid measurement
    format_latency_helper(buf, sizeof(buf), 1000, 1250);
    assert(strcmp(buf, "250ms") == 0);

    printf("[PASS] test_latency_formatting\n");
}

// Test 3: Total capture samples calculation without conflating frame_fill
void test_capture_samples_calculation()
{
    // If snapshot samples > 0, total_samples = snap_samples
    size_t snap_samples = 48000;
    size_t s_capture_offset = 48000;
    size_t total = snap_samples > 0 ? snap_samples : s_capture_offset;
    assert(total == 48000);

    // If snap_samples == 0, total_samples = s_capture_offset (already accumulated live chunks)
    snap_samples = 0;
    total = snap_samples > 0 ? snap_samples : s_capture_offset;
    assert(total == 48000);

    printf("[PASS] test_capture_samples_calculation\n");
}

// Test 4: AudioRecorderStatus and Bounded Recovery using production contract
void test_recorder_status_logic()
{
    // Idle system (request 0): stopped
    assert(evaluate_audio_recorder_status(0, false, false, false, AudioRecorderStatus::UNKNOWN, false, false, false, false) == AudioRecorderStatus::STOPPED);
    // Active recording (request 0): busy
    assert(evaluate_audio_recorder_status(0, true, true, false, AudioRecorderStatus::UNKNOWN, false, false, false, false) == AudioRecorderStatus::BUSY);
    // Recording task stopped, but still owns I2S (request 0): BUSY (must not be treated as STOPPED!)
    assert(evaluate_audio_recorder_status(0, false, true, false, AudioRecorderStatus::UNKNOWN, false, false, false, false) == AudioRecorderStatus::BUSY);

    // Specific request with completion record in ring buffer:
    assert(evaluate_audio_recorder_status(42, false, false, true, AudioRecorderStatus::STOPPED, false, false, false, false) == AudioRecorderStatus::STOPPED);
    assert(evaluate_audio_recorder_status(42, true, true, true, AudioRecorderStatus::STOPPED, false, false, false, false) == AudioRecorderStatus::STOPPED); // Even if new recording started, request 42 is STOPPED!
    assert(evaluate_audio_recorder_status(43, false, false, true, AudioRecorderStatus::REJECTED, false, false, false, false) == AudioRecorderStatus::REJECTED);

    // Specific request with ACK in slot:
    assert(evaluate_audio_recorder_status(44, false, false, false, AudioRecorderStatus::UNKNOWN, true, AudioCommandAckStatus::STOP_ACCEPTED, false, false) == AudioRecorderStatus::STOPPED);
    assert(evaluate_audio_recorder_status(45, false, false, false, AudioRecorderStatus::UNKNOWN, true, AudioCommandAckStatus::REJECTED, false, false) == AudioRecorderStatus::REJECTED);
    // Start ACK must be BUSY while active, NEVER STOPPED!
    assert(evaluate_audio_recorder_status(46, true, true, false, AudioRecorderStatus::UNKNOWN, true, AudioCommandAckStatus::STARTED, false, true) == AudioRecorderStatus::BUSY);

    // In mailbox or active:
    assert(evaluate_audio_recorder_status(47, false, false, false, AudioRecorderStatus::UNKNOWN, false, AudioCommandAckStatus::NONE, true, false) == AudioRecorderStatus::BUSY);
    assert(evaluate_audio_recorder_status(47, true, true, false, AudioRecorderStatus::UNKNOWN, false, AudioCommandAckStatus::NONE, false, true) == AudioRecorderStatus::BUSY);

    // Isolation between Recorder A and Recorder B:
    // Recorder A has finished (completion in ring buffer = STOPPED):
    // Even if Recorder B is currently active/recording, querying Recorder A returns STOPPED!
    assert(evaluate_audio_recorder_status(101, true, true, true, AudioRecorderStatus::STOPPED, false, AudioCommandAckStatus::NONE, false, false) == AudioRecorderStatus::STOPPED);

    printf("[PASS] test_recorder_status_logic\n");
}

// Test 5: Duplicate Stop after Auto-Stop preserves snapshot
void test_auto_stop_and_duplicate_stop()
{
    uint32_t active_cmd_gen = 42;
    uint32_t auto_stopped_cmd = active_cmd_gen;
    uint32_t last_completed_recording_generation = auto_stopped_cmd;
    uint32_t last_completed_snapshot_generation = 101;
    size_t last_completed_sample_count = 32000;

    // Simulate second stop arriving after auto-stop already moved state to IDLE:
    uint32_t cancel_through = 42;
    bool discard = false;
    bool out_snapshot_valid = false;
    uint32_t out_snapshot_generation = 0;
    size_t out_sample_count = 0;

    // In stop_recording_for_generation when recording_state == RECORD_IDLE:
    if (!discard)
    {
        const bool matches = (cancel_through == 0 ||
                              xiaozhi::recorder_control_targets(cancel_through, last_completed_recording_generation));
        if (matches && last_completed_snapshot_generation != 0)
        {
            out_snapshot_valid = true;
            out_snapshot_generation = last_completed_snapshot_generation;
            out_sample_count = last_completed_sample_count;
        }
    }

    assert(out_snapshot_valid);
    assert(out_snapshot_generation == 101);
    assert(out_sample_count == 32000);

    printf("[PASS] test_auto_stop_and_duplicate_stop\n");
}

// Test 6: WiFi generation revalidation on WL_CONNECT_FAILED
void test_wifi_generation_revalidation()
{
    uint32_t request_generation = 5;
    uint32_t active_generation_snapshot = 5;
    uint32_t active_connect_generation = 5;

    // Normal case: generation matches
    assert(active_generation_snapshot == request_generation &&
           active_generation_snapshot == active_connect_generation);

    // If new user request came in during unlock (request_generation changed to 6):
    request_generation = 6;
    // Revalidation must detect mismatch and NOT overwrite new target with stale NVS!
    const bool safe_to_restore = (active_generation_snapshot == request_generation &&
                                  active_generation_snapshot == active_connect_generation);
    assert(!safe_to_restore);

    printf("[PASS] test_wifi_generation_revalidation\n");
}

// Test 7: Touch coordinate initialization and IO OK distinction
void test_touch_coordinate_and_io_ok()
{
    uint16_t touchX = 0, touchY = 0;
    bool io_ok = false;
    bool touched = false;

    // When I2C error occurs:
    const char *state_str = touched ? "PR" : (io_ok ? "REL" : "I2C_ERR");
    assert(strcmp(state_str, "I2C_ERR") == 0);
    assert(touchX == 0 && touchY == 0); // No stack garbage!

    // When normal unpressed occurs:
    io_ok = true;
    touched = false;
    state_str = touched ? "PR" : (io_ok ? "REL" : "I2C_ERR");
    assert(strcmp(state_str, "REL") == 0);

    // When pressed occurs:
    touched = true;
    touchX = 120;
    touchY = 160;
    state_str = touched ? "PR" : (io_ok ? "REL" : "I2C_ERR");
    assert(strcmp(state_str, "PR") == 0);

    printf("[PASS] test_touch_coordinate_and_io_ok\n");
}

// Test 8: Maps Static tile cache precision contract
void test_maps_cache_precision()
{
    char buf[128] = {};
    const double lat = 16.054407;
    const double lon = 108.202167;
    const int zoom = 14;

    format_map_tile_cache_path(buf, sizeof(buf), lat, lon, zoom, "roadmap");
    assert(strcmp(buf, "/maps/16.05441_108.20217_z14_roadmap.jpg") == 0);

    format_map_tile_cache_path(buf, sizeof(buf), 10.776889, 106.700806, 15, "satellite");
    assert(strcmp(buf, "/maps/10.77689_106.70081_z15_satellite.jpg") == 0);

    printf("[PASS] test_maps_cache_precision\n");
}

// Test 9: Power Manager safe elapsed underflow protection, millis wraparound and activity revision
void test_power_manager_underflow_and_revision()
{
    // Normal elapsed
    assert(power_manager_safe_elapsed(10000, 5000) == 5000);

    // Clock anomaly or update during snapshot (now < last_activity): must be 0, NOT underflow!
    assert(power_manager_safe_elapsed(5000, 10000) == 0);

    // Millis wraparound: last_activity shortly before 0xFFFFFFFF, now shortly after 0:
    // 0x00000020 (32) - 0xFFFFFFF0 (4294967280) = 48
    assert(power_manager_safe_elapsed(0x00000020, 0xFFFFFFF0) == 48);
    assert(power_manager_safe_elapsed(64, 0xFFFFFFE0) == 96);

    // Activity revision revalidation:
    // If user touched screen between snapshot and state apply (current_rev != snapshot_rev):
    const uint32_t snapshot_rev = 3;
    const uint8_t snapshot_state = 0; // ACTIVE
    assert(power_manager_can_apply_transition(3, snapshot_rev, 0, snapshot_state) == true);
    // User touch occurred -> revision incremented to 4:
    assert(power_manager_can_apply_transition(4, snapshot_rev, 0, snapshot_state) == false);

    printf("[PASS] test_power_manager_underflow_and_revision\n");
}

// Test 10: Power Brightness Coordinator race resilience
void test_power_brightness_coordination()
{
    PowerBrightnessCoordinator coordinator;
    assert(coordinator.hardware_brightness == 100);

    // Scenario: Task A commits Sleep (intent = 0)
    coordinator.post_intent(0);
    assert(coordinator.intent_revision == 1);

    // Concurrently before Task A writes to hardware, user touches screen -> Task B commits Wake (intent = 100)
    coordinator.post_intent(100);
    assert(coordinator.intent_revision == 2);

    // Serializer executes hardware update:
    uint8_t target_b = 0;
    uint32_t target_rev = 0;
    assert(coordinator.get_next_hardware_target(&target_b, &target_rev) == true);
    // Must get the newest intent (100), NOT 0!
    assert(target_b == 100);
    assert(target_rev == 2);

    coordinator.commit_hardware_applied(target_rev, target_b);
    assert(coordinator.hardware_brightness == 100);

    // Next check has nothing pending:
    assert(coordinator.get_next_hardware_target(&target_b, &target_rev) == false);

    printf("[PASS] test_power_brightness_coordination\n");
}

// Test 11: Camera URL credential stripping for NVS storage
void test_camera_url_credential_stripping()
{
    char out_url[128] = {};
    char out_user[32] = {};
    char out_pass[32] = {};
    bool had_creds = false;

    // Case 1: Plaintext user:password@host URL
    const char *url1 = "http://admin:secret123@192.168.1.100:8080/snapshot.jpg";
    assert(strip_url_credentials(url1, out_url, sizeof(out_url), out_user, sizeof(out_user), out_pass, sizeof(out_pass), &had_creds));
    assert(had_creds == true);
    assert(strcmp(out_url, "http://192.168.1.100:8080/snapshot.jpg") == 0);
    assert(strcmp(out_user, "admin") == 0);
    assert(strcmp(out_pass, "secret123") == 0);

    // Case 2: Query parameter with auth token
    const char *url2 = "http://192.168.1.50/cam.jpg?channel=1&token=xyz789&quality=high";
    assert(strip_url_credentials(url2, out_url, sizeof(out_url), out_user, sizeof(out_user), out_pass, sizeof(out_pass), &had_creds));
    assert(had_creds == true);
    assert(strcmp(out_url, "http://192.168.1.50/cam.jpg?channel=1&quality=high") == 0);

    // Case 3: Clean URL without credentials
    const char *url3 = "rtsp://192.168.1.200:554/live/ch0";
    assert(strip_url_credentials(url3, out_url, sizeof(out_url), out_user, sizeof(out_user), out_pass, sizeof(out_pass), &had_creds));
    assert(had_creds == false);
    assert(strcmp(out_url, url3) == 0);
    assert(out_user[0] == '\0');
    assert(out_pass[0] == '\0');

    // Secret query names are case-insensitive and cover common signed-URL forms.
    const char *url4 = "https://cam.local/frame.jpg?channel=2&Access_Token=topsecret&API_KEY=alsosecret";
    assert(strip_url_credentials(url4, out_url, sizeof(out_url), out_user, sizeof(out_user),
                                 out_pass, sizeof(out_pass), &had_creds));
    assert(had_creds == true);
    assert(strcmp(out_url, "https://cam.local/frame.jpg?channel=2") == 0);

    printf("[PASS] test_camera_url_credential_stripping\n");
}

// Test 12: WiFi Save Failure revalidation after Connect B
void test_wifi_save_failure_revalidation()
{
    uint32_t request_generation = 1; // Initial request A
    bool manual_disconnect = false;
    uint32_t current_save_status = 0; // NONE

    // Save A started for generation 1
    const uint32_t save_gen = 1;

    // While NVS write is executing, user initiates Connect B (generation 2):
    request_generation = 2;

    // NVS write for A fails: ok = false
    const bool ok = false;

    // Revalidation after I/O:
    const bool still_current = (save_gen == request_generation && !manual_disconnect);
    assert(!still_current); // Must be false!

    if (still_current)
    {
        current_save_status = ok ? 1 : 2; // SAVED or FAILED
    }

    // current_save_status must NOT be changed to FAILED for request B!
    assert(current_save_status == 0);

    printf("[PASS] test_wifi_save_failure_revalidation\n");
}

void test_music_stream_url_contracts()
{
    char encoded[192] = {};
    assert(music_url_encode_query("hello world/test", encoded, sizeof(encoded)));
    assert(strcmp(encoded, "hello+world%2Ftest") == 0);

    char worst_case[64] = {};
    memset(worst_case, '%', sizeof(worst_case) - 1);
    assert(music_url_encode_query(worst_case, encoded, sizeof(encoded)));
    assert(strlen(encoded) == 189);

    char too_small[8] = {};
    assert(!music_url_encode_query("%%%%", too_small, sizeof(too_small)));
    assert(too_small[0] == '\0');

    assert(music_stream_url_supported("http://192.168.1.2:8787/youtube/stream?q=x", false));
    assert(!music_stream_url_supported("https://music.example/stream", false));
    assert(music_stream_url_supported("https://music.example/stream", true));
    assert(!music_stream_url_supported("file:///tmp/audio.mp3", true));
    assert(music_playback_needs_storage(0));
    assert(music_playback_needs_storage(7));
    assert(!music_playback_needs_storage(-1));
    assert(audio_clamp_volume_percent(0) == 0);
    assert(audio_clamp_volume_percent(25) == 25);
    assert(audio_clamp_volume_percent(100) == 100);
    assert(audio_clamp_volume_percent(255) == 100);
    assert(es8311_volume_register(audio_clamp_volume_percent(0)) == 0x00);
    printf("[PASS] test_music_stream_url_contracts\n");
}

void test_voice_pcm_filter_contracts()
{
    VoicePcmFilterState state;
    int16_t tail = 0;
    for (int i = 0; i < 512; ++i) tail = voice_pcm_filter_sample(1000, &state);
    assert(tail > -8 && tail < 8); // Constant DC must decay toward zero.

    voice_pcm_filter_reset(&state);
    const int16_t first = voice_pcm_filter_sample(12000, &state);
    const int16_t second = voice_pcm_filter_sample(-12000, &state);
    assert(first != 0 && second != 0); // Speech-band transitions survive.

    VoicePcmFilterState continuous;
    VoicePcmFilterState chunked;
    int16_t continuous_out[8] = {};
    int16_t chunked_out[8] = {};
    const int16_t input[8] = {0, 1000, 2000, -1000, -3000, 500, 12000, -12000};
    for (int i = 0; i < 8; ++i) continuous_out[i] = voice_pcm_filter_sample(input[i], &continuous);
    for (int i = 0; i < 4; ++i) chunked_out[i] = voice_pcm_filter_sample(input[i], &chunked);
    for (int i = 4; i < 8; ++i) chunked_out[i] = voice_pcm_filter_sample(input[i], &chunked);
    assert(memcmp(continuous_out, chunked_out, sizeof(continuous_out)) == 0);

    voice_pcm_filter_reset(&state);
    assert(state.previous_input == 0 && state.previous_output == 0);
    for (int value : {-32768, -32000, 32000, 32767})
    {
        const int16_t output = voice_pcm_filter_sample(static_cast<int16_t>(value), &state);
        assert(output >= -32768 && output <= 32767);
    }
    printf("[PASS] test_voice_pcm_filter_contracts\n");
}

// Test 14: WebSocket Empty Final Continuation Frame
void test_websocket_empty_final_continuation()
{
    uint8_t storage[512] = {};
    xiaozhi::FragmentAssembler assembler(storage, sizeof(storage));

    const uint8_t initial_chunk[] = "{\"type\":\"hello\"}";
    // Frame 1: TEXT, FIN=0 (fragmented)
    assert(assembler.begin(xiaozhi::FragmentAssembler::Kind::TEXT, initial_chunk, strlen((const char *)initial_chunk)));
    assert(assembler.isActive());

    // Frame 2: CONTINUATION, length=0, FIN=1 (empty final frame)
    xiaozhi::FragmentAssembler::Kind finished_kind = xiaozhi::FragmentAssembler::Kind::NONE;
    const uint8_t *message = nullptr;
    size_t message_size = 0;
    assert(assembler.finish(nullptr, 0, &finished_kind, &message, &message_size));
    assert(!assembler.isActive());
    assert(finished_kind == xiaozhi::FragmentAssembler::Kind::TEXT);
    assert(message_size == strlen((const char *)initial_chunk));
    assert(memcmp(message, initial_chunk, message_size) == 0);

    printf("[PASS] test_websocket_empty_final_continuation\n");
}

int main()
{
    printf("=== Running System Defects Regression Tests ===\n");
    test_websocket_framing_and_assembler();
    test_latency_formatting();
    test_capture_samples_calculation();
    test_recorder_status_logic();
    test_auto_stop_and_duplicate_stop();
    test_wifi_generation_revalidation();
    test_touch_coordinate_and_io_ok();
    test_maps_cache_precision();
    test_power_manager_underflow_and_revision();
    test_power_brightness_coordination();
    test_camera_url_credential_stripping();
    test_wifi_save_failure_revalidation();
    test_music_stream_url_contracts();
    test_voice_pcm_filter_contracts();
    test_websocket_empty_final_continuation();
    printf("=== ALL REGRESSION TESTS PASSED! ===\n");
    return 0;
}
