#pragma once

#include <Arduino.h>

enum RuntimeTaskId : uint8_t {
    RUNTIME_TASK_LVGL = 0,
    RUNTIME_TASK_AUDIO,
    RUNTIME_TASK_MUSIC,
    RUNTIME_TASK_WIFI,
    RUNTIME_TASK_MAP,
    RUNTIME_TASK_CAMERA,
    RUNTIME_TASK_XIAOZHI,
    RUNTIME_TASK_MCP,
    RUNTIME_TASK_SETTINGS,
    RUNTIME_TASK_COUNT
};

struct RuntimeTaskHealth {
    uint32_t heartbeat_age_ms;
    uint32_t stack_free_bytes;
    bool seen;
};

void runtime_health_heartbeat(RuntimeTaskId id);
void runtime_health_copy_tasks(RuntimeTaskHealth out[RUNTIME_TASK_COUNT]);
const char *runtime_health_task_name(RuntimeTaskId id);
