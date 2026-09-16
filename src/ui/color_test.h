/**
 * @file color_test.h
 * @brief Display Color Self-Test & Calibration Pattern for ESP32-S3 HMI
 * Dùng để kiểm tra: RGB Order, Panel Invert, 16-bit Color Byte Swap và SPI Signal Integrity.
 */

#pragma once

#include <lvgl.h>

/**
 * @brief Mở màn hình kiểm tra 8 dải màu chuẩn (Black, White, Red, Green, Blue, Cyan, Magenta, Yellow)
 * @param parent Container cha để vẽ giao diện kiểm tra
 */
void ui_color_test_open(lv_obj_t *parent);
void ui_color_test_close(void);
