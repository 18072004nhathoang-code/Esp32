#include "network_coordinator.h"

#include "../ai/ai_voice_service.h"
#include "../audio/music_player.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace
{
SemaphoreHandle_t s_bulk_mutex = nullptr;
}

bool network_coordinator_init(void)
{
    if (s_bulk_mutex) return true;
    s_bulk_mutex = xSemaphoreCreateMutex();
    return s_bulk_mutex != nullptr;
}

bool network_background_allowed(void)
{
    const AIVoiceState voice = ai_voice_get_state();
    const bool voice_realtime = voice == AI_STATE_STARTING || voice == AI_STATE_LISTENING ||
                                voice == AI_STATE_PROCESSING || voice == AI_STATE_SPEAKING ||
                                voice == AI_STATE_CANCELING;
    return !voice_realtime && !music_player_is_playing();
}

bool network_bulk_acquire(uint32_t timeout_ms)
{
    if (!network_background_allowed()) return false;
    SemaphoreHandle_t mutex = s_bulk_mutex;
    if (!mutex) return false;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    if (!network_background_allowed())
    {
        xSemaphoreGive(mutex);
        return false;
    }
    return true;
}

void network_bulk_release(void)
{
    SemaphoreHandle_t mutex = s_bulk_mutex;
    if (mutex) xSemaphoreGive(mutex);
}
