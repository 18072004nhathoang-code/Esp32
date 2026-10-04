#pragma once
#include <stdint.h>

enum class BleRemotePhase : uint8_t { Off, Starting, Advertising, Pairing, Connected, Stopping, Error };
struct BleRemoteSnapshot {
    BleRemotePhase phase;
    bool requested;
    bool passkey_visible;
    uint32_t passkey;
    char name[24];
    char error[96];
};
// Initializes only the small command worker. Radio/host stay off until requested
// in Settings. Control changes never wait for Bluetooth from the LVGL task.
bool ble_remote_init();
void ble_remote_set_enabled(bool enabled);
BleRemoteSnapshot ble_remote_snapshot();
