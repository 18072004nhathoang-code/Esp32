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

// Độ phân giải hiển thị lấy tự động từ Hardware Profile thông qua board_config.h
#ifndef DISP_HOR_RES
#define DISP_HOR_RES BOARD_LCD_WIDTH
#endif

#ifndef DISP_VER_RES
#define DISP_VER_RES BOARD_LCD_HEIGHT
#endif

// Số dòng cho mỗi DMA Buffer (Internal SRAM)
#ifndef DISP_BUF_LINES
#if (DISP_HOR_RES <= 320)
#define DISP_BUF_LINES 40   // 320 * 40 * 2 = 25.6 KB
#else
#define DISP_BUF_LINES 30   // 480 * 30 * 2 = 28.8 KB
#endif
#endif

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

/**
 * @brief Lấy tên định danh của chiều xoay màn hình (Orientation)
 */
const char* display_orientation_name(uint8_t rotation);

typedef struct
{
    bool bgr_order;
    bool inverted;
} DisplayDiagnosticState;

DisplayDiagnosticState lvgl_port_get_display_diagnostic(void);
void lvgl_port_set_display_diagnostic(DisplayDiagnosticState state);
bool lvgl_port_apply_display_diagnostic(void);
const char* lvgl_port_get_color_config_source(void);
