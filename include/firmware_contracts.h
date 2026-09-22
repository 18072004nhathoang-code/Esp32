#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

// This Mini OS build intentionally runs the speaker at full user-scale volume.
// 100% maps to ES8311 unity gain (0 dB, register 0xBF), not the codec's
// positive-gain region above unity.
constexpr uint8_t audio_forced_volume_percent(uint8_t)
{
    return 100U;
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

enum class AudioRecorderStatus : uint8_t
{
    UNKNOWN = 0,
    BUSY,
    REJECTED,
    STOPPED
};

enum class AudioCommandAckStatus : uint8_t
{
    NONE = 0,
    STARTED,
    STOP_ACCEPTED,
    CANCEL_ACCEPTED,
    REJECTED
};

inline AudioRecorderStatus evaluate_audio_recorder_status(
    uint32_t request_id,
    bool is_rec,
    bool owns_i2s,
    bool found_completion,
    AudioRecorderStatus completion_state,
    bool found_ack,
    AudioCommandAckStatus ack_status,
    bool in_mailbox,
    bool is_active_cmd)
{
    if (request_id == 0)
    {
        if (!is_rec && !owns_i2s) return AudioRecorderStatus::STOPPED;
        return AudioRecorderStatus::BUSY;
    }
    if (found_completion)
    {
        return completion_state;
    }
    if (found_ack)
    {
        if (ack_status == AudioCommandAckStatus::REJECTED)
            return AudioRecorderStatus::REJECTED;
        if (ack_status == AudioCommandAckStatus::STOP_ACCEPTED ||
            ack_status == AudioCommandAckStatus::CANCEL_ACCEPTED)
            return AudioRecorderStatus::STOPPED;
        if (ack_status == AudioCommandAckStatus::STARTED)
        {
            if (is_active_cmd && (is_rec || owns_i2s))
                return AudioRecorderStatus::BUSY;
            return AudioRecorderStatus::STOPPED;
        }
    }
    if (in_mailbox || (is_active_cmd && (is_rec || owns_i2s)))
    {
        return AudioRecorderStatus::BUSY;
    }
    return AudioRecorderStatus::UNKNOWN;
}

// Backward compatible overload for boolean ack
inline AudioRecorderStatus evaluate_audio_recorder_status(
    uint32_t request_id,
    bool is_rec,
    bool owns_i2s,
    bool found_completion,
    AudioRecorderStatus completion_state,
    bool found_ack,
    bool ack_ok,
    bool in_mailbox,
    bool is_active_cmd)
{
    return evaluate_audio_recorder_status(
        request_id, is_rec, owns_i2s, found_completion, completion_state,
        found_ack,
        found_ack ? (ack_ok ? AudioCommandAckStatus::STOP_ACCEPTED : AudioCommandAckStatus::REJECTED) : AudioCommandAckStatus::NONE,
        in_mailbox, is_active_cmd);
}

inline int format_map_tile_cache_path(char *buf, size_t buf_size, double lat, double lon, int zoom, const char *type_str)
{
    if (!buf || buf_size == 0) return -1;
    return snprintf(buf, buf_size, "/maps/%.5f_%.5f_z%d_%s.jpg", lat, lon, zoom,
                    (type_str && type_str[0]) ? type_str : "roadmap");
}

constexpr uint32_t power_manager_safe_elapsed(uint32_t now, uint32_t last_activity)
{
    // In 32-bit modulo arithmetic, if (now - last_activity) > 0x7FFFFFFF,
    // last_activity is ahead of now (clock anomaly or concurrent activity feed during snapshot)
    return ((now - last_activity) > 0x7FFFFFFFU) ? 0U : (now - last_activity);
}

constexpr bool power_manager_can_apply_transition(
    uint32_t current_revision,
    uint32_t snapshot_revision,
    uint8_t current_state,
    uint8_t snapshot_state)
{
    return (current_revision == snapshot_revision) && (current_state == snapshot_state);
}

struct PowerBrightnessCoordinator
{
    uint32_t intent_revision{0};
    uint32_t applied_revision{0};
    uint8_t target_brightness{100};
    uint8_t hardware_brightness{100};

    void post_intent(uint8_t brightness)
    {
        target_brightness = brightness;
        ++intent_revision;
    }

    bool get_next_hardware_target(uint8_t *out_brightness, uint32_t *out_revision)
    {
        if (intent_revision == applied_revision) return false;
        *out_brightness = target_brightness;
        *out_revision = intent_revision;
        return true;
    }

    void commit_hardware_applied(uint32_t revision, uint8_t brightness)
    {
        if (revision > applied_revision)
        {
            applied_revision = revision;
            hardware_brightness = brightness;
        }
    }
};

inline bool strip_url_credentials(const char *src_url,
                                  char *out_url, size_t out_size,
                                  char *out_user, size_t user_size,
                                  char *out_pass, size_t pass_size,
                                  bool *had_credentials)
{
    if (had_credentials) *had_credentials = false;
    if (out_user && user_size > 0) out_user[0] = '\0';
    if (out_pass && pass_size > 0) out_pass[0] = '\0';
    if (!out_url || out_size == 0) return false;
    out_url[0] = '\0';
    if (!src_url || !*src_url) return true;

    // Check scheme
    const char *scheme_sep = strstr(src_url, "://");
    const char *host_start = scheme_sep ? (scheme_sep + 3) : src_url;

    // Look for userinfo ('@' before next '/' or '?')
    const char *slash = strchr(host_start, '/');
    const char *query = strchr(host_start, '?');
    const char *authority_end = slash ? slash : (query ? query : (src_url + strlen(src_url)));
    const char *at = strchr(host_start, '@');

    size_t out_idx = 0;
    if (scheme_sep)
    {
        size_t scheme_len = (scheme_sep + 3) - src_url;
        if (scheme_len >= out_size) return false;
        memcpy(out_url, src_url, scheme_len);
        out_idx = scheme_len;
        out_url[out_idx] = '\0';
    }

    const char *host_actual = host_start;
    if (at && at < authority_end)
    {
        if (had_credentials) *had_credentials = true;
        // Userinfo present: [user][:password]@
        const char *colon = strchr(host_start, ':');
        if (colon && colon < at)
        {
            // Both username and password
            if (out_user && user_size > 0)
            {
                size_t ulen = colon - host_start;
                if (ulen >= user_size) ulen = user_size - 1;
                memcpy(out_user, host_start, ulen);
                out_user[ulen] = '\0';
            }
            if (out_pass && pass_size > 0)
            {
                size_t plen = at - (colon + 1);
                if (plen >= pass_size) plen = pass_size - 1;
                memcpy(out_pass, colon + 1, plen);
                out_pass[plen] = '\0';
            }
        }
        else
        {
            // Only username
            if (out_user && user_size > 0)
            {
                size_t ulen = at - host_start;
                if (ulen >= user_size) ulen = user_size - 1;
                memcpy(out_user, host_start, ulen);
                out_user[ulen] = '\0';
            }
        }
        host_actual = at + 1;
    }

    // Copy host and path up to query
    const char *query_start = strchr(host_actual, '?');
    const char *copy_end = query_start ? query_start : (src_url + strlen(src_url));

    size_t host_path_len = copy_end - host_actual;
    if (out_idx + host_path_len >= out_size) return false;
    memcpy(out_url + out_idx, host_actual, host_path_len);
    out_idx += host_path_len;
    out_url[out_idx] = '\0';

    // Parse and filter query parameters
    if (query_start)
    {
        const char *p = query_start + 1;
        bool first_param = true;
        while (*p)
        {
            const char *next_amp = strchr(p, '&');
            size_t param_len = next_amp ? (next_amp - p) : strlen(p);

            const char *eq = strchr(p, '=');
            size_t key_len = (eq && eq < (p + param_len)) ? (eq - p) : param_len;

            bool is_secret = false;
            const char *secret_keys[] = {"token", "pass", "password", "pwd", "auth", "key", "secret"};
            for (const char *sk : secret_keys)
            {
                if (strlen(sk) == key_len && strncmp(p, sk, key_len) == 0)
                {
                    is_secret = true;
                    if (had_credentials) *had_credentials = true;
                    if (eq && out_pass && out_pass[0] == '\0' && pass_size > 0 &&
                        (strcmp(sk, "pass") == 0 || strcmp(sk, "password") == 0 || strcmp(sk, "pwd") == 0))
                    {
                        size_t val_len = (p + param_len) - (eq + 1);
                        if (val_len >= pass_size) val_len = pass_size - 1;
                        memcpy(out_pass, eq + 1, val_len);
                        out_pass[val_len] = '\0';
                    }
                    break;
                }
            }

            if (!is_secret)
            {
                if (out_idx + 1 + param_len >= out_size) return false;
                out_url[out_idx++] = first_param ? '?' : '&';
                first_param = false;
                memcpy(out_url + out_idx, p, param_len);
                out_idx += param_len;
                out_url[out_idx] = '\0';
            }

            if (!next_amp) break;
            p = next_amp + 1;
        }
    }
    return true;
}

