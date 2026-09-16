#pragma once

#include <Arduino.h>

bool tjpg_guard_init(void);
bool tjpg_guard_lock(uint32_t timeout_ms = 1000);
void tjpg_guard_unlock(void);

