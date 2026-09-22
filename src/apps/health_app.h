#pragma once
#include <lvgl.h>
#include "../os/system_info.h"

void health_app_open(lv_obj_t *parent);
void health_app_close(void);
void health_app_update(const SystemStats &stats);
