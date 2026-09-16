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
 * @brief Lưu cấu hình IP Camera hiện tại vào NVS
 */
bool camera_service_save_network_profile(void);

/**
 * @brief Lấy cấu hình IP Camera hiện tại (bản sao an toàn luồng)
 */
NetworkCameraProfile camera_service_get_network_profile(void);

/**
 * @brief Lấy trạng thái runtime hiện tại của camera
 */
CameraRuntimeState camera_service_get_runtime_state(void);

/**
 * @brief Kiểm tra camera đã kết nối và sẵn sàng truyền frame
 */
bool camera_service_is_connected(void);

/**
 * @brief Khởi chạy luồng bắt hình / phát video
 */
bool camera_service_start(void);

/**
 * @brief Dừng luồng bắt hình
 */
bool camera_service_stop(uint32_t timeout_ms = 2000);

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

/**
 * @brief Truy vấn trạng thái thực tế của từng tính năng
 */
CameraFeatureStatus camera_service_get_snapshot_status(void);
CameraFeatureStatus camera_service_get_mjpeg_status(void);
CameraFeatureStatus camera_service_get_rtsp_status(void);
CameraFeatureStatus camera_service_get_onvif_status(void);
CameraFeatureStatus camera_service_get_local_dvp_status(void);
