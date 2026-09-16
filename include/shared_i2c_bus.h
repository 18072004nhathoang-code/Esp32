/**
 * @file shared_i2c_bus.h
 * @brief Quản lý Shared I2C Bus đồng bộ giữa Touch Controller (FT6336G) và Audio Codec (ES8311)
 * Đảm bảo cùng 1 physical bus dùng chung 1 controller driver (Wire) và có Mutex bảo vệ tránh collision.
 */

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo physical I2C bus một lần duy nhất tại boot.
 * @return true nếu khởi tạo phần cứng I2C thành công
 */
bool shared_i2c_init(void);

/**
 * @brief Khóa I2C bus trước khi giao tiếp phần cứng.
 */
bool shared_i2c_lock(uint32_t timeout_ms = 100);

/**
 * @brief Mở khóa I2C bus sau khi hoàn tất giao tiếp.
 */
void shared_i2c_unlock(void);

/**
 * @brief Kiểm tra sự hiện diện của thiết bị I2C theo địa chỉ (Probe thật).
 */
bool shared_i2c_probe(uint8_t dev_addr);

/**
 * @brief Ghi 1 byte vào thanh ghi I2C (đã bảo vệ bằng mutex).
 */
bool shared_i2c_write_reg(uint8_t dev_addr, uint8_t reg, uint8_t val);

/**
 * @brief Đọc 1 chuỗi byte từ thanh ghi I2C (đã bảo vệ bằng mutex).
 */
bool shared_i2c_read_reg(uint8_t dev_addr, uint8_t reg, uint8_t *data, size_t len);

/**
 * @brief Trạng thái phát hiện thực tế của chip Touch FT6336G tại boot.
 */
bool shared_i2c_touch_is_detected(void);

/**
 * @brief Trạng thái phát hiện thực tế của chip Codec ES8311 tại boot.
 */
bool shared_i2c_codec_is_detected(void);

/**
 * @brief Đọc tọa độ cảm ứng từ chip FT6336G và map theo rotation của board.
 * @param x Con trỏ nhận tọa độ X (đã chuẩn hóa theo rotation)
 * @param y Con trỏ nhận tọa độ Y (đã chuẩn hóa theo rotation)
 * @return true nếu đang có ngón tay chạm vào màn hình
 */
bool shared_i2c_touch_read(uint16_t *x, uint16_t *y);

/**
 * @brief Đọc tọa độ cảm ứng chi tiết bao gồm cả raw và mapped (dùng cho Touch Test).
 */
bool shared_i2c_touch_read_debug(uint16_t *raw_x, uint16_t *raw_y, uint16_t *mapped_x, uint16_t *mapped_y);

#ifdef __cplusplus
}
#endif
