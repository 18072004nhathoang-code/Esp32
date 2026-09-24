#include "runtime_health.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace
{
struct TaskSample {
    uint32_t heartbeat_ms;
    uint32_t stack_free_bytes;
    bool seen;
    bool active;
};
TaskSample s_samples[RUNTIME_TASK_COUNT] = {};
uint32_t s_events[RUNTIME_EVENT_COUNT] = {};
portMUX_TYPE s_health_mux = portMUX_INITIALIZER_UNLOCKED;
}

void runtime_health_heartbeat(RuntimeTaskId id)
{
    if (id >= RUNTIME_TASK_COUNT) return;
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_health_mux);
    const bool sample_stack = !s_samples[id].seen ||
                              now - s_samples[id].heartbeat_ms >= 1000U;
    if (!sample_stack)
    {
        s_samples[id].heartbeat_ms = now;
        s_samples[id].active = true;
    }
    portEXIT_CRITICAL(&s_health_mux);
    if (!sample_stack) return;

    const uint32_t stack = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    portENTER_CRITICAL(&s_health_mux);
    // uxTaskGetStackHighWaterMark is already a lifetime minimum for the
    // current task. Keep the minimum across task restarts as well so a short
    // camera/export/self-test worker cannot disappear from soak evidence.
    if (s_samples[id].stack_free_bytes == 0 || stack < s_samples[id].stack_free_bytes)
        s_samples[id].stack_free_bytes = stack;
    // Publish seen only after its first valid stack sample, preventing a
    // concurrent health snapshot from observing seen=true with stack=0.
    s_samples[id].heartbeat_ms = now;
    s_samples[id].seen = true;
    s_samples[id].active = true;
    portEXIT_CRITICAL(&s_health_mux);
}

void runtime_health_task_finished(RuntimeTaskId id)
{
    if (id >= RUNTIME_TASK_COUNT) return;
    portENTER_CRITICAL(&s_health_mux);
    // Preserve `seen` and the historical stack low-water after a dynamic task
    // exits. Only active tasks participate in heartbeat-stall detection.
    s_samples[id].active = false;
    s_samples[id].heartbeat_ms = 0;
    portEXIT_CRITICAL(&s_health_mux);
}

void runtime_health_copy_tasks(RuntimeTaskHealth out[RUNTIME_TASK_COUNT])
{
    if (!out) return;
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_health_mux);
    for (uint8_t i = 0; i < RUNTIME_TASK_COUNT; ++i)
    {
        out[i].seen = s_samples[i].seen;
        out[i].stack_free_bytes = s_samples[i].stack_free_bytes;
        out[i].active = s_samples[i].active;
        out[i].heartbeat_age_ms = s_samples[i].active
            ? now - s_samples[i].heartbeat_ms : UINT32_MAX;
    }
    portEXIT_CRITICAL(&s_health_mux);
}

void runtime_health_count_event(RuntimeEventId id)
{
    if (id >= RUNTIME_EVENT_COUNT) return;
    portENTER_CRITICAL(&s_health_mux);
    if (s_events[id] != UINT32_MAX) ++s_events[id];
    portEXIT_CRITICAL(&s_health_mux);
}

void runtime_health_copy_events(uint32_t out[RUNTIME_EVENT_COUNT])
{
    if (!out) return;
    portENTER_CRITICAL(&s_health_mux);
    for (uint8_t i = 0; i < RUNTIME_EVENT_COUNT; ++i)
        out[i] = s_events[i];
    portEXIT_CRITICAL(&s_health_mux);
}

const char *runtime_health_task_name(RuntimeTaskId id)
{
    static const char *names[RUNTIME_TASK_COUNT] = {
        "Main", "LVGL", "Audio", "Music", "WiFi", "Map", "Camera",
        "CameraUI", "Xiaozhi", "MCP", "Settings", "RecorderExport",
        "SpeakerTest", "MusicStress"
    };
    return id < RUNTIME_TASK_COUNT ? names[id] : "Unknown";
}
