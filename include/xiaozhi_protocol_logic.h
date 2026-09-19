#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace xiaozhi
{
static const uint16_t kMaxOpusPacketBytes = 1275;
static const size_t kMaxJsonMessageBytes = 16U * 1024U;

enum class LifecycleState : uint8_t
{
    INACTIVE = 0,
    PROVISIONING,
    NEEDS_ACTIVATION,
    IDLE,
    CONNECTING,
    LISTENING,
    PROCESSING,
    SPEAKING,
    CANCELING,
    ERROR
};

struct ProvisionedWebsocket
{
    char url[256];
    char token[256];
    uint8_t version;
};

inline bool is_secure_websocket_url(const char *url)
{
    return url && strncmp(url, "wss://", 6) == 0 && url[6] != '\0';
}

inline bool valid_websocket_config(const ProvisionedWebsocket &config)
{
    return is_secure_websocket_url(config.url) && config.token[0] != '\0' &&
           config.version >= 1 && config.version <= 3;
}

inline bool session_event_is_current(uint32_t event_generation,
                                     uint32_t active_generation,
                                     uint32_t cancelled_through)
{
    return event_generation != 0 && event_generation == active_generation &&
           event_generation > cancelled_through;
}

inline bool bounded_enqueue_allowed(size_t count, size_t capacity)
{
    return capacity != 0 && count < capacity;
}

inline uint32_t clamp_activation_poll_ms(uint32_t requested_ms)
{
    return requested_ms < 3000U ? 3000U :
           (requested_ms > 60000U ? 60000U : requested_ms);
}

inline bool deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

enum class SessionFault : uint8_t
{
    NONE = 0,
    STALE,
    CANCELLED,
    HELLO_TIMEOUT,
    DISCONNECTED,
    QUEUE_OVERFLOW
};

inline SessionFault session_fault(bool current, bool cancelled, bool waiting_hello,
                                  bool connected, bool deadline_expired,
                                  uint32_t dropped_uplink, uint32_t dropped_downlink)
{
    if (!current) return SessionFault::STALE;
    if (cancelled) return SessionFault::CANCELLED;
    if (dropped_uplink || dropped_downlink) return SessionFault::QUEUE_OVERFLOW;
    if (waiting_hello && deadline_expired) return SessionFault::HELLO_TIMEOUT;
    if (!waiting_hello && !connected) return SessionFault::DISCONNECTED;
    return SessionFault::NONE;
}

inline uint16_t read_be16(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t read_be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

inline void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

inline void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}

inline size_t wrap_opus_packet(uint8_t version, uint32_t timestamp_ms,
                               const uint8_t *opus, size_t opus_size,
                               uint8_t *output, size_t output_capacity)
{
    if (!opus || !output || opus_size == 0 || opus_size > kMaxOpusPacketBytes) return 0;
    if (version == 1)
    {
        if (output_capacity < opus_size) return 0;
        memcpy(output, opus, opus_size);
        return opus_size;
    }
    if (version == 2)
    {
        const size_t header = 16;
        if (output_capacity < header + opus_size) return 0;
        write_be16(output, 2);
        write_be16(output + 2, 0);
        write_be32(output + 4, 0);
        write_be32(output + 8, timestamp_ms);
        write_be32(output + 12, static_cast<uint32_t>(opus_size));
        memcpy(output + header, opus, opus_size);
        return header + opus_size;
    }
    if (version == 3)
    {
        const size_t header = 4;
        if (output_capacity < header + opus_size) return 0;
        output[0] = 0;
        output[1] = 0;
        write_be16(output + 2, static_cast<uint16_t>(opus_size));
        memcpy(output + header, opus, opus_size);
        return header + opus_size;
    }
    return 0;
}

inline bool unwrap_opus_packet(uint8_t version, const uint8_t *packet,
                               size_t packet_size, const uint8_t **opus,
                               size_t *opus_size, uint32_t *timestamp_ms)
{
    if (!packet || !opus || !opus_size || !timestamp_ms || packet_size == 0) return false;
    *timestamp_ms = 0;
    if (version == 1)
    {
        if (packet_size > kMaxOpusPacketBytes) return false;
        *opus = packet;
        *opus_size = packet_size;
        return true;
    }
    if (version == 2)
    {
        if (packet_size < 16 || read_be16(packet) != 2 || read_be16(packet + 2) != 0 ||
            read_be32(packet + 4) != 0) return false;
        const size_t declared = read_be32(packet + 12);
        if (declared == 0 || declared > kMaxOpusPacketBytes || declared != packet_size - 16)
            return false;
        *timestamp_ms = read_be32(packet + 8);
        *opus = packet + 16;
        *opus_size = declared;
        return true;
    }
    if (version == 3)
    {
        if (packet_size < 4 || packet[0] != 0 || packet[1] != 0) return false;
        const size_t declared = read_be16(packet + 2);
        if (declared == 0 || declared > kMaxOpusPacketBytes || declared != packet_size - 4)
            return false;
        *opus = packet + 4;
        *opus_size = declared;
        return true;
    }
    return false;
}

class FragmentAssembler
{
public:
    enum class Kind : uint8_t { NONE = 0, TEXT, BINARY };

    FragmentAssembler(uint8_t *storage, size_t capacity)
        : storage_(storage), capacity_(capacity), size_(0), kind_(Kind::NONE), failed_(false) {}

    bool begin(Kind kind, const uint8_t *data, size_t size)
    {
        reset();
        if (kind == Kind::NONE) return false;
        kind_ = kind;
        return append(data, size);
    }

    bool append(const uint8_t *data, size_t size)
    {
        if (kind_ == Kind::NONE || failed_ || !data || size > capacity_ - size_)
        {
            failed_ = true;
            return false;
        }
        memcpy(storage_ + size_, data, size);
        size_ += size;
        return true;
    }

    bool finish(const uint8_t *data, size_t size, Kind *kind,
                const uint8_t **message, size_t *message_size)
    {
        if (!append(data, size) || !kind || !message || !message_size) return false;
        *kind = kind_;
        *message = storage_;
        *message_size = size_;
        kind_ = Kind::NONE;
        return true;
    }

    void reset() { size_ = 0; kind_ = Kind::NONE; failed_ = false; }
    bool failed() const { return failed_; }

private:
    uint8_t *storage_;
    size_t capacity_;
    size_t size_;
    Kind kind_;
    bool failed_;
};

class DuplicateRequestTracker
{
public:
    DuplicateRequestTracker() : next_(0)
    {
        memset(valid_, 0, sizeof(valid_));
        memset(ids_, 0, sizeof(ids_));
    }
    bool seen(uint32_t id) const
    {
        for (size_t i = 0; i < kCapacity; ++i)
        {
            if (valid_[i] && ids_[i] == id) return true;
        }
        return false;
    }
    bool remember(uint32_t id)
    {
        if (seen(id)) return false;
        valid_[next_] = true;
        ids_[next_] = id;
        next_ = (next_ + 1) % kCapacity;
        return true;
    }
    void reset()
    {
        memset(valid_, 0, sizeof(valid_));
        next_ = 0;
    }
private:
    static const size_t kCapacity = 8;
    bool valid_[kCapacity];
    uint32_t ids_[kCapacity];
    size_t next_;
};

struct MusicHandoff
{
    bool paused_by_voice;
    bool suppress_resume;
};

enum class McpTool : uint8_t
{
    UNKNOWN = 0,
    MUSIC_PLAY,
    MUSIC_PAUSE,
    MUSIC_RESUME,
    MUSIC_STOP,
    MUSIC_VOLUME,
    CAMERA_OPEN,
    CAMERA_START,
    CAMERA_STOP,
    CAMERA_REFRESH,
    CAMERA_STATUS,
    CLOCK_TIME
};

inline McpTool mcp_tool_type(const char *name)
{
    if (!name) return McpTool::UNKNOWN;
    if (strcmp(name, "self.music.play") == 0) return McpTool::MUSIC_PLAY;
    if (strcmp(name, "self.music.pause") == 0) return McpTool::MUSIC_PAUSE;
    if (strcmp(name, "self.music.resume") == 0) return McpTool::MUSIC_RESUME;
    if (strcmp(name, "self.music.stop") == 0) return McpTool::MUSIC_STOP;
    if (strcmp(name, "self.music.set_volume") == 0) return McpTool::MUSIC_VOLUME;
    if (strcmp(name, "self.camera.open") == 0) return McpTool::CAMERA_OPEN;
    if (strcmp(name, "self.camera.start") == 0) return McpTool::CAMERA_START;
    if (strcmp(name, "self.camera.stop") == 0) return McpTool::CAMERA_STOP;
    if (strcmp(name, "self.camera.refresh") == 0) return McpTool::CAMERA_REFRESH;
    if (strcmp(name, "self.camera.get_status") == 0) return McpTool::CAMERA_STATUS;
    if (strcmp(name, "self.clock.get_time") == 0) return McpTool::CLOCK_TIME;
    return McpTool::UNKNOWN;
}

inline bool mcp_volume_valid(int value) { return value >= 0 && value <= 100; }
inline bool mcp_result_success(bool command_enqueued, bool device_ack)
{
    return command_enqueued && device_ack;
}

inline bool should_resume_music(const MusicHandoff &handoff, bool session_cancelled)
{
    (void)session_cancelled;
    return handoff.paused_by_voice && !handoff.suppress_resume;
}

enum class HelloValidationResult : uint8_t
{
    OK = 0,
    INVALID_TRANSPORT,
    INVALID_FORMAT,
    INVALID_CHANNELS,
    UNSUPPORTED_SAMPLE_RATE,
    INVALID_SESSION_ID
};

inline HelloValidationResult validate_server_hello(const char *transport,
                                                   const char *format,
                                                   int channels,
                                                   uint32_t sample_rate,
                                                   const char *session_id,
                                                   size_t max_session_len)
{
    if (!transport || strcmp(transport, "websocket") != 0)
        return HelloValidationResult::INVALID_TRANSPORT;
    if (!format || strcmp(format, "opus") != 0)
        return HelloValidationResult::INVALID_FORMAT;
    if (channels != 1)
        return HelloValidationResult::INVALID_CHANNELS;
    if (sample_rate != 8000 && sample_rate != 12000 && sample_rate != 16000 &&
        sample_rate != 24000 && sample_rate != 48000)
        return HelloValidationResult::UNSUPPORTED_SAMPLE_RATE;
    if (!session_id || !*session_id || strlen(session_id) >= max_session_len)
        return HelloValidationResult::INVALID_SESSION_ID;
    return HelloValidationResult::OK;
}

inline const char *hello_validation_error_string(HelloValidationResult res)
{
    switch (res)
    {
        case HelloValidationResult::INVALID_TRANSPORT: return "Hello transport không phải websocket";
        case HelloValidationResult::INVALID_FORMAT: return "Hello audio format không phải opus";
        case HelloValidationResult::INVALID_CHANNELS: return "Hello audio channels không phải mono (1)";
        case HelloValidationResult::UNSUPPORTED_SAMPLE_RATE: return "Hello sample rate không được hỗ trợ";
        case HelloValidationResult::INVALID_SESSION_ID: return "Hello session_id trống hoặc quá dài";
        default: return "Hello hợp lệ";
    }
}

inline bool is_mcp_async_tool(McpTool tool)
{
    switch (tool)
    {
        case McpTool::MUSIC_PLAY:
        case McpTool::MUSIC_PAUSE:
        case McpTool::MUSIC_RESUME:
        case McpTool::MUSIC_STOP:
        case McpTool::MUSIC_VOLUME:
        case McpTool::CAMERA_OPEN:
        case McpTool::CAMERA_START:
        case McpTool::CAMERA_STOP:
        case McpTool::CAMERA_REFRESH:
            return true;
        default:
            return false;
    }
}
}

