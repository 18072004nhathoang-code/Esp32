#include "ble_remote_logic.h"
#include "display_dma_policy.h"
#include <assert.h>
#include <stdio.h>
#include <fstream>
using namespace ble_remote;

struct Device : Ports {
    State state;
    bool ack = true;
    int calls = 0;
    Op applied = Op::Status;
    uint16_t value = 0;
    Device() { state.available = state.sd = true; state.tracks = 4; state.track = 0; }
    State snapshot() override { return state; }
    bool apply(Op op, uint16_t v) override {
        ++calls; applied = op; value = v;
        if (ack && op == Op::Play) { state.playing = true; state.track = static_cast<int16_t>(v); }
        if (ack && op == Op::Volume) state.volume = static_cast<uint8_t>(v);
        return ack;
    }
};
int main() {
    const size_t strip = display_dma::bytes(240, display_dma::kPortraitLines, 2);
    assert(strip == 3840 && display_dma::bytes(240, 40, 2) - strip == 15360);
    assert(startup_ram_ok(72U * 1024U, 32U * 1024U));
    assert(!startup_ram_ok(72U * 1024U - 1, 32U * 1024U));
    assert(!startup_ram_ok(100U * 1024U, 32U * 1024U - 1));
    assert(!runtime_ram_ok(20848, 12788)); // measured first BLE init must roll back
    assert(runtime_ram_ok(32U * 1024U, 12U * 1024U));
    assert(!runtime_ram_ok(32U * 1024U - 1, 12U * 1024U));
    assert(!runtime_ram_ok(64U * 1024U, 12U * 1024U - 1));
    Command c = {};
    uint8_t bytes[6] = {1, 1, 1, 0, 3, 0};
    assert(decode(bytes, 6, c) && c.id == 1 && c.value == 3);
    assert(!decode(nullptr, 6, c) && !decode(bytes, 5, c) && !decode(bytes, 7, c));
    for (unsigned op = 0; op < 256; ++op) {
        uint8_t raw[6] = {1, static_cast<uint8_t>(op), 1, 0, 0, 0};
        assert(decode(raw, 6, c) == (op >= 1 && op <= 8));
    }
    bytes[0] = 2; assert(!decode(bytes, 6, c)); bytes[0] = 1;
    bytes[2] = 0; assert(!decode(bytes, 6, c)); bytes[2] = 1;
    bytes[1] = 5; bytes[4] = 101; assert(!decode(bytes, 6, c));
    bytes[4] = 100; assert(decode(bytes, 6, c));
    bytes[1] = 4; assert(!decode(bytes, 6, c));
    Device device; State after;
    c = {Op::Play, 1, 3};
    assert(execute(c, device, after) == Result::Applied && device.calls == 1 && after.track == 3);
    c.value = 4; assert(execute(c, device, after) == Result::Invalid && device.calls == 1);
    c = {Op::Next, 2, 0}; assert(execute(c, device, after) == Result::Applied && device.value == 0);
    c.op = Op::Previous; assert(execute(c, device, after) == Result::Applied && device.value == 3);
    device.state.busy = true; c.op = Op::Stop;
    int before = device.calls;
    assert(execute(c, device, after) == Result::Busy && device.calls == before);
    c.op = Op::Status; assert(execute(c, device, after) == Result::Status && device.calls == before);
    device.state.busy = false; device.state.sd = false; c.op = Op::Play;
    assert(execute(c, device, after) == Result::Unavailable && device.calls == before);
    device.state.sd = true; device.state.available = false; c.op = Op::Volume;
    assert(execute(c, device, after) == Result::Unavailable && device.calls == before);
    device.state.available = true; device.ack = false; c.op = Op::Stop;
    assert(execute(c, device, after) == Result::NotConfirmed); // no fake success on timeout/queue failure
    uint8_t encoded[20] = {};
    after.track = -1; after.position = 0x12345678U; after.session = 0xabcdef12U;
    encode(after, 0x1234, Result::NotConfirmed, encoded);
    assert(encoded[0] == 1 && encoded[2] == 0x34 && encoded[3] == 0x12 && encoded[4] == 4);
    assert(encoded[6] == 255 && encoded[8] == 0x78 && encoded[11] == 0x12);
    assert(encoded[16] == 0x12 && encoded[19] == 0xab);
    // One shared wire fixture is decoded by the phone tests and encoded by the
    // firmware implementation, so an independent JS model cannot hide drift.
    State golden;
    golden.available = golden.playing = golden.wifi = golden.sd = true;
    golden.volume = 50; golden.track = 2; golden.tracks = 4;
    golden.position = 0x12345678U; golden.duration = 90; golden.session = 0xabcdef12U;
    encode(golden, 0x1234, Result::NotConfirmed, encoded);
    std::ifstream fixture("tests/fixtures/ble_status_v1.txt");
    assert(fixture.good());
    for (unsigned i = 0, byte = 0; i < sizeof(encoded); ++i) {
        fixture >> std::hex >> byte; assert(!fixture.fail() && byte == encoded[i]);
    }
    Gate gate; uint32_t epoch = 0;
    assert(!gate.reserve(1, epoch));
    gate.connect(); assert(!gate.reserve(1, epoch)); gate.authenticate(true);
    assert(gate.reserve(1, epoch) && !gate.reserve(2, epoch));
    gate.rollback(epoch); assert(gate.reserve(1, epoch)); // queue-full retry, ID not committed
    assert(gate.finish(epoch, 1) && !gate.reserve(1, epoch)); // no duplicate actions
    assert(gate.reserve(2, epoch)); uint32_t old = epoch;
    gate.disconnect(); assert(!gate.authorized(old) && !gate.finish(old, 2));
    gate.connect(); gate.authenticate(true); assert(gate.reserve(1, epoch));
    gate.rollback(old); assert(gate.pending()); // stale callback cannot wipe a new command
    assert(!gate.finish(old, 2) && gate.finish(epoch, 1));
    gate.authenticate(false); assert(!gate.reserve(2, epoch));
    puts("BLE production codec/dispatch/session tests PASS");
}
