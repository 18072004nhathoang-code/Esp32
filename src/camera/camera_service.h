/**
 * @file camera_service.h
 * @brief Tầng dịch vụ trừu tượng quản lý Camera / RTSP Video Streamer cho ESP32-S3
 */

#pragma once

#include "camera_types.h"

/**
 * @brief Khởi tạo hệ thống Camera theo cấu hình phần cứng
 * @param config Cấu hình chân và thông số khung hình
 * @return true nếu tìm thấy phần cứng và khởi tạo thành công
 */
bool camera_service_init(const CameraConfig &config);

/**
 * @brief Bắt đầu luồng bắt hình (Capture Stream)
 */
bool camera_service_start(void);

/**
 * @brief Dừng luồng bắt hình
 */
void camera_service_stop(void);

/**
 * @brief Lấy khung hình mới nhất từ Frame Buffer
 * @param timeout_ms Thời gian chờ tối đa
 * @return Con trỏ tới CameraFrame hoặc nullptr nếu chưa có khung hình mới
 */
CameraFrame* camera_service_get_frame(uint32_t timeout_ms = 1000);

/**
 * @brief Trả lại khung hình sau khi xử lý hoặc render xong để tái sử dụng bộ đệm
 * @param frame Khung hình cần trả
 */
void camera_service_return_frame(CameraFrame *frame);

/**
 * @brief Kiểm tra xem phần cứng Camera có sẵn sàng không
 */
bool camera_service_is_available(void);

/**
 * @brief Lấy tên định danh của cảm biến camera hiện tại
 */
const char* camera_service_get_model_name(void);
