#pragma once

#include <stddef.h>

void time_service_init(void);
void time_service_update(void);
bool time_service_is_synced(void);
bool time_service_format_clock(char *buffer, size_t size);

