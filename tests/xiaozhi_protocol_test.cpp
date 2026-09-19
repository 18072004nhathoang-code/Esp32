#include "xiaozhi_protocol_logic.h"

#include <assert.h>
#include <string.h>

int main()
{
    xiaozhi::ProvisionedWebsocket activation = {};
    strcpy(activation.url, "wss://example.test/xiaozhi/v1/");
    strcpy(activation.token, "token-from-activation");
    activation.version = 1;
    assert(xiaozhi::valid_websocket_config(activation));
    strcpy(activation.url, "ws://example.test/insecure");
    assert(!xiaozhi::valid_websocket_config(activation));
    strcpy(activation.url, "wss://example.test/");
    activation.token[0] = '\0';
    assert(!xiaozhi::valid_websocket_config(activation));

    const uint8_t opus[] = {0x11, 0x22, 0x33, 0x44};
    for (uint8_t version = 1; version <= 3; ++version)
    {
        uint8_t packet[64] = {};
        const size_t packet_size = xiaozhi::wrap_opus_packet(
            version, 0x12345678, opus, sizeof(opus), packet, sizeof(packet));
        assert(packet_size > 0);
        const uint8_t *decoded = nullptr;
        size_t decoded_size = 0;
        uint32_t timestamp = 0;
        assert(xiaozhi::unwrap_opus_packet(version, packet, packet_size,
                                           &decoded, &decoded_size, &timestamp));
        assert(decoded_size == sizeof(opus));
        assert(memcmp(decoded, opus, sizeof(opus)) == 0);
        assert(version != 2 || timestamp == 0x12345678);
        packet[packet_size - 1] ^= 0xff;
        assert(xiaozhi::unwrap_opus_packet(version, packet, packet_size,
                                           &decoded, &decoded_size, &timestamp));
    }

    uint8_t malformed_v3[] = {0, 0, 0, 5, 1, 2};
    const uint8_t *decoded = nullptr;
    size_t decoded_size = 0;
    uint32_t timestamp = 0;
    assert(!xiaozhi::unwrap_opus_packet(3, malformed_v3, sizeof(malformed_v3),
                                        &decoded, &decoded_size, &timestamp));

    uint8_t fragment_buffer[16] = {};
    xiaozhi::FragmentAssembler fragments(fragment_buffer, sizeof(fragment_buffer));
    const uint8_t a[] = {'{', '"', 'a'};
    const uint8_t b[] = {'"', ':', '1'};
    const uint8_t c[] = {'}'};
    assert(fragments.begin(xiaozhi::FragmentAssembler::Kind::TEXT, a, sizeof(a)));
    assert(fragments.append(b, sizeof(b)));
    xiaozhi::FragmentAssembler::Kind kind = xiaozhi::FragmentAssembler::Kind::NONE;
    const uint8_t *message = nullptr;
    size_t message_size = 0;
    assert(fragments.finish(c, sizeof(c), &kind, &message, &message_size));
    assert(kind == xiaozhi::FragmentAssembler::Kind::TEXT);
    assert(message_size == 7 && memcmp(message, "{\"a\":1}", 7) == 0);
    const uint8_t overflow[17] = {};
    assert(!fragments.begin(xiaozhi::FragmentAssembler::Kind::BINARY,
                            overflow, sizeof(overflow)));

    assert(xiaozhi::bounded_enqueue_allowed(0, 8));
    assert(!xiaozhi::bounded_enqueue_allowed(8, 8));
    assert(xiaozhi::clamp_activation_poll_ms(1) == 3000);
    assert(xiaozhi::clamp_activation_poll_ms(10000) == 10000);
    assert(xiaozhi::clamp_activation_poll_ms(120000) == 60000);
    assert(!xiaozhi::deadline_reached(99, 100));
    assert(xiaozhi::deadline_reached(100, 100));
    assert(xiaozhi::deadline_reached(5, 0xfffffff0U));
    assert(!xiaozhi::session_event_is_current(4, 5, 3));
    assert(!xiaozhi::session_event_is_current(5, 5, 5));
    assert(xiaozhi::session_event_is_current(6, 6, 5));
    assert(xiaozhi::session_fault(false, false, false, true, false, 0, 0) ==
           xiaozhi::SessionFault::STALE);
    assert(xiaozhi::session_fault(true, true, false, true, false, 0, 0) ==
           xiaozhi::SessionFault::CANCELLED);
    assert(xiaozhi::session_fault(true, false, true, true, true, 0, 0) ==
           xiaozhi::SessionFault::HELLO_TIMEOUT);
    assert(xiaozhi::session_fault(true, false, false, false, false, 0, 0) ==
           xiaozhi::SessionFault::DISCONNECTED);
    assert(xiaozhi::session_fault(true, false, false, true, false, 1, 0) ==
           xiaozhi::SessionFault::QUEUE_OVERFLOW);

    xiaozhi::DuplicateRequestTracker duplicate;
    assert(!duplicate.seen(0));
    assert(duplicate.remember(0));
    assert(duplicate.seen(0));
    assert(!duplicate.remember(0));
    assert(duplicate.remember(42));
    assert(duplicate.seen(42));
    assert(!duplicate.remember(42));

    xiaozhi::MusicHandoff handoff = {true, false};
    assert(xiaozhi::should_resume_music(handoff, false));
    handoff.suppress_resume = true;
    assert(!xiaozhi::should_resume_music(handoff, true));

    assert(xiaozhi::mcp_tool_type("self.music.play") == xiaozhi::McpTool::MUSIC_PLAY);
    assert(xiaozhi::mcp_tool_type("self.system.reboot") == xiaozhi::McpTool::UNKNOWN);
    assert(!xiaozhi::mcp_volume_valid(-1));
    assert(xiaozhi::mcp_volume_valid(100));
    assert(!xiaozhi::mcp_result_success(true, false));
    assert(xiaozhi::mcp_result_success(true, true));

    // Server hello validation
    assert(xiaozhi::validate_server_hello("websocket", "opus", 1, 16000, "sess-123", 96) ==
           xiaozhi::HelloValidationResult::OK);
    assert(xiaozhi::validate_server_hello("webrtc", "opus", 1, 16000, "sess-123", 96) ==
           xiaozhi::HelloValidationResult::INVALID_TRANSPORT);
    assert(xiaozhi::validate_server_hello("websocket", "pcm", 1, 16000, "sess-123", 96) ==
           xiaozhi::HelloValidationResult::INVALID_FORMAT);
    assert(xiaozhi::validate_server_hello("websocket", "opus", 2, 16000, "sess-123", 96) ==
           xiaozhi::HelloValidationResult::INVALID_CHANNELS);
    assert(xiaozhi::validate_server_hello("websocket", "opus", 1, 44100, "sess-123", 96) ==
           xiaozhi::HelloValidationResult::UNSUPPORTED_SAMPLE_RATE);
    assert(xiaozhi::validate_server_hello("websocket", "opus", 1, 16000, "", 96) ==
           xiaozhi::HelloValidationResult::INVALID_SESSION_ID);
    assert(xiaozhi::validate_server_hello("websocket", "opus", 1, 16000, nullptr, 96) ==
           xiaozhi::HelloValidationResult::INVALID_SESSION_ID);

    // MCP Async Tools
    assert(xiaozhi::is_mcp_async_tool(xiaozhi::McpTool::MUSIC_PLAY));
    assert(xiaozhi::is_mcp_async_tool(xiaozhi::McpTool::MUSIC_STOP));
    assert(xiaozhi::is_mcp_async_tool(xiaozhi::McpTool::CAMERA_OPEN));
    assert(xiaozhi::is_mcp_async_tool(xiaozhi::McpTool::CAMERA_STOP));
    assert(!xiaozhi::is_mcp_async_tool(xiaozhi::McpTool::CAMERA_STATUS));
    assert(!xiaozhi::is_mcp_async_tool(xiaozhi::McpTool::CLOCK_TIME));
    assert(!xiaozhi::is_mcp_async_tool(xiaozhi::McpTool::UNKNOWN));

    return 0;
}
