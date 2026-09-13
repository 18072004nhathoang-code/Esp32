/**
 * @file lvgl_port.h
 * @brief Tầng kết nối (Porting Layer) giữa LVGL 8 và LovyanGFX trên FreeRTOS ESP32-S3
 */

#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include "LGFX_Config.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Độ phân giải hiển thị chuẩn Mini OS trên màn hình 3.5" IPS (Landscape 480x320)
#define DISP_HOR_RES 480
#define DISP_VER_RES 320

// Số dòng cho mỗi DMA Buffer (480 * 30 * 2 = 28.8 KB trong Internal SRAM)
#define DISP_BUF_LINES 30

// Đối tượng phần cứng hiển thị LovyanGFX
extern LGFX gfx;

// Mutex bảo vệ luồng an toàn (Thread-Safe) cho giao diện LVGL
extern SemaphoreHandle_t lvgl_mutex;

/**
 * @brief Khóa Mutex trước khi gọi các hàm LVGL từ các Task nền
 * @param timeout_ms Thời gian chờ tối đa (mặc định portMAX_DELAY)
 * @return true nếu lấy được khóa, false nếu hết thời gian chờ
 */
bool lvgl_port_lock(uint32_t timeout_ms = portMAX_DELAY);

/**
 * @brief Mở khóa Mutex sau khi hoàn tất tác vụ LVGL
 */
void lvgl_port_unlock(void);

/**
 * @brief Khởi tạo toàn bộ hệ thống đồ họa LovyanGFX, bộ nhớ đệm DMA và Task LVGL
 * @return true nếu khởi tạo thành công
 */
bool lvgl_port_init(void);

/**
 * @brief Điều chỉnh độ sáng đèn nền màn hình (0 - 100%)
 */
void lvgl_port_set_brightness(uint8_t percent);

/**
 * @brief Lấy phần trăm độ sáng hiện tại
 */
uint8_t lvgl_port_get_brightness(void);
