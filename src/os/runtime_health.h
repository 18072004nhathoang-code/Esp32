#pragma once

#include <Arduino.h>

enum RuntimeTaskId : uint8_t {
    RUNTIME_TASK_MAIN = 0,
    RUNTIME_TASK_LVGL,
    RUNTIME_TASK_AUDIO,
    RUNTIME_TASK_MUSIC,
    RUNTIME_TASK_WIFI,
    RUNTIME_TASK_MAP,
    RUNTIME_TASK_CAMERA,
    RUNTIME_TASK_CAMERA_UI,
    RUNTIME_TASK_XIAOZHI,
    RUNTIME_TASK_MCP,
    RUNTIME_TASK_SETTINGS,
    RUNTIME_TASK_RECORDER_EXPORT,
    RUNTIME_TASK_SPEAKER_TEST,
    RUNTIME_TASK_MUSIC_STRESS,
    RUNTIME_TASK_COUNT
};

enum RuntimeEventId : uint8_t {
    RUNTIME_EVENT_WIFI_LOSS = 0,
    RUNTIME_EVENT_WIFI_RECOVERY,
    RUNTIME_EVENT_MAP_OPEN,
    RUNTIME_EVENT_MAP_CLOSE,
    RUNTIME_EVENT_MAP_REQUEST,
    RUNTIME_EVENT_MAP_PUBLISH,
    RUNTIME_EVENT_MAP_STALE_DROP,
    RUNTIME_EVENT_CAMERA_OPEN,
    RUNTIME_EVENT_CAMERA_CLOSE,
    RUNTIME_EVENT_CAMERA_FRAME,
    RUNTIME_EVENT_VOICE_START,
    RUNTIME_EVENT_VOICE_LISTENING,
    RUNTIME_EVENT_VOICE_COMPLETE,
    RUNTIME_EVENT_VOICE_FAULT,
    RUNTIME_EVENT_COUNT
};

struct RuntimeTaskHealth {
    uint32_t heartbeat_age_ms;
    uint32_t stack_free_bytes;
    bool seen;
    bool active;
};

void runtime_health_heartbeat(RuntimeTaskId id);
void runtime_health_task_finished(RuntimeTaskId id);
void runtime_health_copy_tasks(RuntimeTaskHealth out[RUNTIME_TASK_COUNT]);
void runtime_health_count_event(RuntimeEventId id);
void runtime_health_copy_events(uint32_t out[RUNTIME_EVENT_COUNT]);
const char *runtime_health_task_name(RuntimeTaskId id);
