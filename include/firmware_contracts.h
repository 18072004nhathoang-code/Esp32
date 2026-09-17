#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

inline bool ui_screen_to_local(int32_t screen_x, int32_t screen_y,
                               int32_t origin_x, int32_t origin_y,
                               uint16_t width, uint16_t height,
                               int16_t *local_x, int16_t *local_y)
{
    if (!local_x || !local_y) return false;
    const int32_t x = screen_x - origin_x;
    const int32_t y = screen_y - origin_y;
    if (x < 0 || y < 0 || x >= width || y >= height) return false;
    *local_x = static_cast<int16_t>(x);
    *local_y = static_cast<int16_t>(y);
    return true;
}

constexpr bool bounded_body_append_allowed(size_t current, size_t incoming, size_t limit)
{
    return current <= limit && incoming <= limit - current;
}

constexpr bool http_dechunked_body_complete(int expected_length, size_t actual_length,
                                            int stream_result, bool sink_failed)
{
    return stream_result >= 0 && !sink_failed && actual_length > 0 &&
           (expected_length < 0 || actual_length == static_cast<size_t>(expected_length));
}

constexpr size_t audio_stereo_frames_from_bytes(size_t bytes)
{
    return bytes / (2U * sizeof(int16_t));
}

constexpr size_t audio_rx_carry_after_bytes(size_t bytes)
{
    return bytes % (2U * sizeof(int16_t));
}

constexpr bool audio_write_completed(size_t sent_bytes, size_t expected_bytes)
{
    return expected_bytes > 0 && sent_bytes == expected_bytes;
}

// ES8311 DAC volume is -95.5 dB at 0x00, 0 dB at 0xBF and +32 dB at 0xFF.
// Keep the user range at/below unity gain; zero is handled as a real mute.
constexpr uint8_t es8311_volume_register(uint8_t percent)
{
    return percent == 0 ? 0U
                        : static_cast<uint8_t>(0x47U +
                              ((static_cast<uint32_t>(percent > 100 ? 100 : percent) - 1U) *
                               (0xBFU - 0x47U)) / 99U);
}

constexpr bool audio_session_cleanup_allowed(uint32_t cleanup_session, uint32_t active_session)
{
    return cleanup_session != 0 && cleanup_session == active_session;
}

inline bool valid_utf8_text(const char *text, size_t length)
{
    if (!text && length != 0) return false;
    for (size_t i = 0; i < length;)
    {
        const uint8_t lead = static_cast<uint8_t>(text[i++]);
        if (lead <= 0x7F)
        {
            if (lead < 0x20 && lead != '\n' && lead != '\r' && lead != '\t') return false;
            continue;
        }
        uint32_t codepoint = 0;
        size_t continuation = 0;
        uint32_t minimum = 0;
        if ((lead & 0xE0) == 0xC0) { codepoint = lead & 0x1F; continuation = 1; minimum = 0x80; }
        else if ((lead & 0xF0) == 0xE0) { codepoint = lead & 0x0F; continuation = 2; minimum = 0x800; }
        else if ((lead & 0xF8) == 0xF0) { codepoint = lead & 0x07; continuation = 3; minimum = 0x10000; }
        else return false;
        if (i + continuation > length) return false;
        for (size_t n = 0; n < continuation; ++n)
        {
            const uint8_t byte = static_cast<uint8_t>(text[i++]);
            if ((byte & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (byte & 0x3F);
        }
        if (codepoint < minimum || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;
    }
    return true;
}

constexpr bool exclusive_start_can_claim(uint8_t state, uint8_t idle_state)
{
    return state == idle_state;
}

inline uint16_t contract_le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t contract_le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct PcmWavView
{
    const int16_t *samples;
    size_t sample_count;
};

inline bool parse_pcm16_mono_16k_wav(const uint8_t *body, size_t body_size, PcmWavView *view)
{
    if (!body || !view || body_size < 44 || memcmp(body, "RIFF", 4) != 0 ||
        memcmp(body + 8, "WAVE", 4) != 0) return false;
    const uint32_t riff_size = contract_le32(body + 4);
    if (static_cast<uint64_t>(riff_size) + 8U != body_size) return false;

    bool fmt_valid = false;
    const uint8_t *pcm = nullptr;
    size_t pcm_bytes = 0;
    size_t offset = 12;
    while (offset + 8 <= body_size)
    {
        const uint8_t *chunk = body + offset;
        const uint32_t chunk_size = contract_le32(chunk + 4);
        offset += 8;
        if (chunk_size > body_size - offset) return false;
        if (memcmp(chunk, "fmt ", 4) == 0)
        {
            if (chunk_size < 16) return false;
            fmt_valid = contract_le16(body + offset) == 1 &&
                        contract_le16(body + offset + 2) == 1 &&
                        contract_le32(body + offset + 4) == 16000 &&
                        contract_le32(body + offset + 8) == 32000 &&
                        contract_le16(body + offset + 12) == 2 &&
                        contract_le16(body + offset + 14) == 16;
        }
        else if (memcmp(chunk, "data", 4) == 0)
        {
            if ((chunk_size & 1U) != 0) return false;
            pcm = body + offset;
            pcm_bytes = chunk_size;
        }
        const size_t padded = static_cast<size_t>(chunk_size) + (chunk_size & 1U);
        if (padded > body_size - offset) return false;
        offset += padded;
    }
    if (offset != body_size || !fmt_valid || !pcm || pcm_bytes == 0) return false;
    view->samples = reinterpret_cast<const int16_t *>(pcm);
    view->sample_count = pcm_bytes / sizeof(int16_t);
    return true;
}

constexpr bool request_response_is_current(uint32_t response_id, uint32_t active_id,
                                           uint32_t cancelled_through)
{
    return response_id != 0 && response_id == active_id && response_id > cancelled_through;
}

constexpr bool camera_session_accepts(uint32_t frame_session, uint32_t active_session, bool active)
{
    return active && frame_session != 0 && frame_session == active_session;
}

constexpr bool camera_control_needs_apply(uint32_t requested_revision, uint32_t acknowledged_revision)
{
    return requested_revision != acknowledged_revision;
}

constexpr bool camera_control_can_ack(bool operation_ok, bool target_running,
                                      bool running_or_starting, bool stopped)
{
    return operation_ok && (target_running ? running_or_starting : stopped);
}

constexpr bool wifi_generation_can_commit(uint32_t save_generation, uint32_t current_generation,
                                          uint32_t pending_generation, bool manual_disconnect)
{
    return save_generation != 0 && save_generation == current_generation &&
           save_generation == pending_generation && !manual_disconnect;
}

constexpr uint8_t estimated_cpu_usage_from_rates(uint64_t idle_rate, uint64_t idle_capacity_rate)
{
    return idle_capacity_rate == 0 ? 0U
        : static_cast<uint8_t>(100U - (idle_rate >= idle_capacity_rate
            ? 100U : static_cast<uint32_t>((idle_rate * 100ULL) / idle_capacity_rate)));
}

constexpr bool complete_jpeg_signature(const uint8_t *data, size_t size)
{
    return data && size >= 4 && data[0] == 0xFF && data[1] == 0xD8 &&
           data[size - 2] == 0xFF && data[size - 1] == 0xD9;
}

constexpr bool cache_temp_can_replace(bool exact_write, bool exact_size, bool backup_ready)
{
    return exact_write && exact_size && backup_ready;
}
