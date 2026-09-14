/**
 * @file spi_bus_guard.h
 * @brief Cơ chế khóa đồng bộ Mutex bảo vệ SPI Bus (FSPI GPIO 11/12/13)
 * dùng chung giữa màn hình LCD ST7796 (LovyanGFX DMA) và thẻ nhớ MicroSD.
 */

#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo Mutex bảo vệ bus SPI dùng chung
 */
void spi_bus_guard_init(void);

/**
 * @brief Chiếm quyền sử dụng bus SPI (chờ DMA màn hình hoàn tất trước khi cấp quyền)
 * @param timeout_ms Thời gian chờ tối đa (ms)
 * @return true nếu chiếm quyền thành công
 */
bool spi_bus_lock(uint32_t timeout_ms = 1000);

/**
 * @brief Giải phóng quyền sử dụng bus SPI
 */
void spi_bus_unlock(void);

#ifdef __cplusplus
}
#endif
