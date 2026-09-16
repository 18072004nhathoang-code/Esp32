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

/** Force the existing calibration screen to start a new attempt. */
void ui_touch_test_request_forced_calibration(void);

/** True when startup should open calibration before any network UI. */
bool ui_touch_test_should_auto_open(void);

/** True while normal app navigation and touch delivery must stay locked. */
bool ui_touch_test_is_calibration_blocking(void);

#ifdef __cplusplus
}
#endif
