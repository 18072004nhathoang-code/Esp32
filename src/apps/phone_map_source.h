#pragma once

#include <Arduino.h>

struct PhoneMapFrameMetadata
{
    uint32_t generation;
    double lat;
    double lon;
    int zoom;
    char maptype[16];
};

bool phone_map_source_init(void);
bool phone_map_source_set_request(uint32_t generation, double lat, double lon,
                                  int zoom, const char *maptype);
void phone_map_source_clear_request(void);
bool phone_map_source_take_frame(uint8_t *dest, size_t capacity,
                                 size_t *out_size,
                                 PhoneMapFrameMetadata *out_metadata);
void phone_map_source_get_pairing_hint(char *out, size_t out_size);
