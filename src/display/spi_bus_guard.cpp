/**
 * @file spi_bus_guard.cpp
 * @brief Triển khai cơ chế khóa đồng bộ Mutex bảo vệ SPI Bus (FSPI GPIO 11/12/13)
 * dùng chung giữa màn hình LCD ST7796 (LovyanGFX DMA) và thẻ nhớ MicroSD.
 */

#include "spi_bus_guard.h"
#include "lvgl_port.h"

static SemaphoreHandle_t spi_shared_mutex = nullptr;

void spi_bus_guard_init(void)
{
    if (!spi_shared_mutex)
    {
        spi_shared_mutex = xSemaphoreCreateMutex();
    }
}

bool spi_bus_lock(uint32_t timeout_ms)
{
    if (!spi_shared_mutex)
    {
        spi_bus_guard_init();
    }

    if (spi_shared_mutex && xSemaphoreTake(spi_shared_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        // Đồng bộ hoàn tất mọi transaction DMA đang dở trên LovyanGFX
        gfx.waitDMA();
        return true;
    }
    return false;
}

void spi_bus_unlock(void)
{
    if (spi_shared_mutex)
    {
        xSemaphoreGive(spi_shared_mutex);
    }
}
