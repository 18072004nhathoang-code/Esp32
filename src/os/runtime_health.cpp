#include "runtime_health.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace
{
struct TaskSample { uint32_t heartbeat_ms; uint32_t stack_free_bytes; bool seen; };
TaskSample s_samples[RUNTIME_TASK_COUNT] = {};
portMUX_TYPE s_health_mux = portMUX_INITIALIZER_UNLOCKED;
}

void runtime_health_heartbeat(RuntimeTaskId id)
{
    if (id >= RUNTIME_TASK_COUNT) return;
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_health_mux);
    const bool sample_stack = !s_samples[id].seen ||
                              now - s_samples[id].heartbeat_ms >= 1000U;
    s_samples[id].heartbeat_ms = now;
    s_samples[id].seen = true;
    portEXIT_CRITICAL(&s_health_mux);
    if (!sample_stack) return;

    const uint32_t stack = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    portENTER_CRITICAL(&s_health_mux);
    s_samples[id].stack_free_bytes = stack;
    portEXIT_CRITICAL(&s_health_mux);
}

void runtime_health_task_finished(RuntimeTaskId id)
{
    if (id >= RUNTIME_TASK_COUNT) return;
    portENTER_CRITICAL(&s_health_mux);
    s_samples[id] = {};
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
        out[i].heartbeat_age_ms = s_samples[i].seen ? now - s_samples[i].heartbeat_ms : UINT32_MAX;
    }
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
