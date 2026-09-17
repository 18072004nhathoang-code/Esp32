#pragma once

#include <stddef.h>
#include <stdint.h>

struct TouchContactDecision
{
    int8_t index;
    bool release_before_switch;
};

// Pure contact-selection logic shared by the FT6336 driver and firmware tests.
// A missing active ID always produces one release sample before another ID can
// become active, preventing a second finger from inheriting the first gesture.
inline TouchContactDecision touch_contact_select(uint8_t active_id,
                                                 const uint8_t *ids,
                                                 const uint8_t *events,
                                                 size_t count)
{
    if (!ids || !events) return {-1, active_id != 0xFF};
    if (active_id != 0xFF)
    {
        for (size_t i = 0; i < count; ++i)
            if (ids[i] == active_id && events[i] <= 2) return {static_cast<int8_t>(i), false};
        return {-1, true};
    }
    for (size_t i = 0; i < count; ++i)
        if (events[i] <= 2) return {static_cast<int8_t>(i), false};
    return {-1, false};
}

