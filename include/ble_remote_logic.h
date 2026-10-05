#pragma once

#include <stddef.h>
#include <stdint.h>

// Little-endian, fixed-size packets fit the mandatory ATT MTU of 23.
namespace ble_remote {
constexpr size_t kCommandBytes = 6;
constexpr size_t kStatusBytes = 20;
constexpr bool startup_ram_ok(size_t free_bytes, size_t largest) {
    return free_bytes >= 72U * 1024U && largest >= 32U * 1024U;
}
constexpr bool runtime_ram_ok(size_t free_bytes, size_t largest) {
    return free_bytes >= 32U * 1024U && largest >= 12U * 1024U;
}
enum class Op : uint8_t { Play = 1, Pause, Resume, Stop, Volume, Status, Next, Previous };
enum class Result : uint8_t { Status = 0, Applied, Invalid, Busy, NotConfirmed, Unavailable };
struct Command { Op op; uint16_t id; uint16_t value; };
struct State {
    bool available = false;
    bool playing = false;
    bool paused = false;
    bool wifi = false;
    bool sd = false;
    bool busy = false;
    uint8_t volume = 0;
    int16_t track = -1;
    uint16_t tracks = 0;
    uint32_t position = 0;
    uint32_t duration = 0;
    uint32_t session = 0; // wire generation: rejects stale notifications after reconnect
};
bool decode(const uint8_t *data, size_t length, Command &out);
void encode(const State &state, uint16_t id, Result result, uint8_t out[kStatusBytes]);
struct Ports {
    virtual ~Ports() {}
    virtual State snapshot() = 0;
    // Only true after the existing decoder task ACK, not enqueue success.
    virtual bool apply(Op op, uint16_t value) = 0;
};
Result execute(const Command &command, Ports &ports, State &after);

// Caller serializes access. Connection generations invalidate queued work and
// results; one outstanding command and monotonic IDs prevent duplicate playback.
class Gate {
public:
    uint32_t connect() { disconnect(); connected_ = true; return epoch_; }
    void disconnect() { ++epoch_; connected_ = authenticated_ = pending_ = false; last_ = 0; }
    void authenticate(bool value) { authenticated_ = connected_ && value; }
    bool authorized(uint32_t epoch) const { return epoch == epoch_ && connected_ && authenticated_; }
    bool reserve(uint16_t id, uint32_t &epoch) {
        if (!authorized(epoch_) || pending_ || id == 0 || id <= last_) return false;
        pending_ = true; pending_id_ = id; epoch = epoch_; return true;
    }
    void rollback(uint32_t epoch) { if (epoch == epoch_) pending_ = false; }
    bool finish(uint32_t epoch, uint16_t id) {
        if (!authorized(epoch) || !pending_ || id != pending_id_) return false;
        last_ = id; pending_ = false; return true;
    }
    bool pending() const { return pending_; }
    uint32_t epoch() const { return epoch_; }
private:
    uint32_t epoch_ = 0;
    uint16_t last_ = 0, pending_id_ = 0;
    bool connected_ = false, authenticated_ = false, pending_ = false;
};
} // namespace ble_remote
