#include "tjpg_guard.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_tjpg_mutex = nullptr;

bool tjpg_guard_init(void)
{
    if (!s_tjpg_mutex)
    {
        s_tjpg_mutex = xSemaphoreCreateMutex();
    }
    if (!s_tjpg_mutex)
    {
        Serial.println("[TJPG] ❌ Chế độ suy giảm: không tạo được mutex decoder");
        return false;
    }
    return true;
}

bool tjpg_guard_lock(uint32_t timeout_ms)
{
    return tjpg_guard_init() &&
           xSemaphoreTake(s_tjpg_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void tjpg_guard_unlock(void)
{
    if (s_tjpg_mutex) xSemaphoreGive(s_tjpg_mutex);
}

