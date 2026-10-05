#include "ble_remote_logic.h"

namespace ble_remote {
static uint16_t u16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, uint16_t(v)); put16(p + 2, uint16_t(v >> 16)); }
bool decode(const uint8_t *data, size_t length, Command &out) {
    if (!data || length != kCommandBytes || data[0] != 1 || data[1] < 1 || data[1] > 8) return false;
    const Command value = {static_cast<Op>(data[1]), u16(data + 2), u16(data + 4)};
    if (!value.id || (value.op == Op::Volume && value.value > 100) ||
        (value.op != Op::Play && value.op != Op::Volume && value.value != 0)) return false;
    out = value;
    return true;
}
void encode(const State &s, uint16_t id, Result result, uint8_t out[kStatusBytes]) {
    out[0] = 1;
    out[1] = (s.playing ? 1 : 0) | (s.paused ? 2 : 0) | (s.wifi ? 4 : 0) |
             (s.sd ? 8 : 0) | (s.busy ? 16 : 0) | (!s.available ? 32 : 0);
    put16(out + 2, id); out[4] = static_cast<uint8_t>(result); out[5] = s.volume;
    out[6] = static_cast<uint8_t>(s.track); out[7] = static_cast<uint8_t>(s.tracks);
    put32(out + 8, s.position); put32(out + 12, s.duration);
    put32(out + 16, s.session);
}
Result execute(const Command &c, Ports &ports, State &after) {
    after = ports.snapshot();
    if (c.op == Op::Status) return Result::Status;
    if (!after.available) return Result::Unavailable;
    if (after.busy) return Result::Busy;
    Op op = c.op;
    uint16_t value = c.value;
    if (op == Op::Play || op == Op::Next || op == Op::Previous) {
        if (!after.sd || !after.tracks) return Result::Unavailable;
        if (op != Op::Play) {
            const int32_t current = after.track < 0 ? 0 : after.track;
            value = uint16_t((current + after.tracks + (op == Op::Next ? 1 : -1)) % after.tracks);
            op = Op::Play;
        }
        if (value >= after.tracks) return Result::Invalid;
    }
    const bool ack = ports.apply(op, value);
    after = ports.snapshot();
    return ack ? Result::Applied : Result::NotConfirmed;
}
} // namespace ble_remote
