/**
 * @file camera_service.h
 * @brief Tầng dịch vụ thống nhất quản lý Camera cho UI (Vendor-Agnostic Facade)
 * Kết nối LocalCameraService (DVP) và NetworkCameraService (IP Camera Hikvision, KBVision, Ezviz, Yoosee, ONVIF)
 */

#pragma once

#include "camera_types.h"
#include "local_camera_service.h"
#include "network_camera_service.h"

/**
 * @brief Khởi tạo hệ thống Camera
 */
bool camera_service_init(void);

/**
 * @brief Chuyển đổi nguồn camera (DVP cục bộ hoặc IP Cam qua mạng)
 */
void camera_service_set_source(CameraSourceType source);
CameraSourceType camera_service_get_source(void);

/**
 * @brief Cấu hình thông số IP Camera qua mạng theo profile nhà sản xuất
 */
bool camera_service_configure_network(const NetworkCameraProfile &profile);

/**
 * @brief Khởi chạy luồng bắt hình / phát video
 */
bool camera_service_start(void);

/**
 * @brief Dừng luồng bắt hình
 */
void camera_service_stop(void);

/**
 * @brief Lấy khung hình mới nhất
 */
CameraFrame* camera_service_get_frame(uint32_t timeout_ms = 1000);

/**
 * @brief Trả lại khung hình sau khi vẽ/render xong
 */
void camera_service_return_frame(CameraFrame *frame);

/**
 * @brief Kiểm tra xem nguồn camera hiện tại có sẵn sàng không
 */
bool camera_service_is_available(void);

/**
 * @brief Lấy chuỗi mô tả trạng thái camera hiện tại cho UI
 */
const char* camera_service_get_status_text(void);

/**
 * @brief Lấy tên model sensor hoặc luồng camera hiện tại
 */
const char* camera_service_get_model_name(void);
