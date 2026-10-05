#pragma once
#include "navigation_protocol.h"
struct NavigationSnapshot {
    navigation::Guidance guidance;
    uint32_t session=0,sequence=0,revision=0;
    bool active=false,fresh=false,awake=false;
    navigation::Result result=navigation::Received;
};
bool navigation_init();
bool navigation_receive(const uint8_t *data,size_t size);
void navigation_disconnect();
bool navigation_snapshot(NavigationSnapshot &out);
void navigation_ack(uint8_t out[20]);
bool navigation_audio_busy();
void navigation_audio_owner_pump();
void navigation_capture_music();
