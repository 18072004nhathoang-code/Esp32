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
