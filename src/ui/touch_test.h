/**
 * @file touch_test.h
 * @brief Touch Diagnostic & Calibration Pattern for ESP32-S3 HMI (Landscape 320x240)
 * Dùng để kiểm tra: Tọa độ Raw X/Y, Mapped X/Y, Crosshair thời gian thực, test 4 góc + center.
 */

#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mở màn hình kiểm tra cảm ứng Touch Test
 * @param parent Container cha
 */
void ui_touch_test_open(lv_obj_t *parent);

/**
 * @brief Cập nhật định kỳ tọa độ và crosshair (gọi từ ui_manager_update)
 */
void ui_touch_test_update(void);

/**
 * @brief Đóng và dọn dẹp touch test
 */
void ui_touch_test_close(void);

#ifdef __cplusplus
}
#endif
