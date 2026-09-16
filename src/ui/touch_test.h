/**
 * @file touch_test.h
 * @brief Raw FT6336 five-point affine calibration and touch diagnostic.
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
