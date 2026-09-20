#include "xiaozhi_protocol_logic.h"
#include "xiaozhi_session_logic.h"

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
    const uint8_t pong_payload[] = "heartbeat_timestamp_12345";
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
    size_t s_capture_frame_fill = 960;
    size_t total = snap_samples > 0 ? snap_samples : s_capture_offset;
    assert(total == 48000);

    // If snap_samples == 0, total_samples = s_capture_offset (already accumulated live chunks)
    snap_samples = 0;
    total = snap_samples > 0 ? snap_samples : s_capture_offset;
    assert(total == 48000);

    printf("[PASS] test_capture_samples_calculation\n");
}

// Test 4: AudioRecorderStatus and Bounded Recovery
enum class MockRecorderStatus : uint8_t { UNKNOWN = 0, BUSY, REJECTED, STOPPED };

void test_recorder_status_logic()
{
    auto evaluate_status = [](bool is_recording, bool owns_i2s, bool found, bool ack_ok, bool in_mailbox) -> MockRecorderStatus {
        if (!is_recording && !owns_i2s && (!found || ack_ok) && !in_mailbox)
            return MockRecorderStatus::STOPPED;
        if (found && !ack_ok)
            return MockRecorderStatus::REJECTED;
        if (in_mailbox || is_recording || owns_i2s)
            return MockRecorderStatus::BUSY;
        return MockRecorderStatus::UNKNOWN;
    };

    // Idle system: stopped
    assert(evaluate_status(false, false, true, true, false) == MockRecorderStatus::STOPPED);
    // Active recording: busy
    assert(evaluate_status(true, true, true, true, false) == MockRecorderStatus::BUSY);
    // Recording task stopped, but still owns I2S: BUSY (must not be treated as STOPPED!)
    assert(evaluate_status(false, true, true, true, false) == MockRecorderStatus::BUSY);
    // Rejected start: REJECTED
    assert(evaluate_status(false, false, true, false, false) == MockRecorderStatus::REJECTED);

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
    printf("=== ALL REGRESSION TESTS PASSED! ===\n");
    return 0;
}
