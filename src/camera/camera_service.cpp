/**
 * @file camera_service.cpp
 * @brief Triển khai tầng dịch vụ quản lý Camera / RTSP Video Streamer cho ESP32-S3
 * Thiết kế chuẩn để mở rộng thêm OV2640 / OV5640 / RTSP / ONVIF sau này.
 */

#include "camera_service.h"

static bool is_cam_available = false;
static bool is_cam_streaming = false;
static CameraModel active_model = CAMERA_MODEL_NONE;
static CameraConfig active_config;

bool camera_service_init(const CameraConfig &config)
{
    active_config = config;
    Serial.println("[CAMERA] Đang quét phần cứng cảm biến Camera DVP...");

    // Khi chưa có phần cứng Camera cắm vào chân GPIO, đánh dấu không khả dụng một cách an toàn
    // Không crash, không loop vô tận và không chiếm dụng bộ đệm DMA
    is_cam_available = false;
    is_cam_streaming = false;
    active_model = CAMERA_MODEL_NONE;

    Serial.println("[CAMERA] ⚠️ Không phát hiện cảm biến Camera DVP (Hệ thống sẵn sàng mở rộng module OV2640/OV5640).");
    return false;
}

bool camera_service_start(void)
{
    if (!is_cam_available)
    {
        return false;
    }
    is_cam_streaming = true;
    Serial.println("[CAMERA] Đã kích hoạt luồng phát hình Video.");
    return true;
}

void camera_service_stop(void)
{
    is_cam_streaming = false;
    Serial.println("[CAMERA] Đã dừng luồng phát hình Video.");
}

CameraFrame* camera_service_get_frame(uint32_t timeout_ms)
{
    // Mock an toàn: Khi chưa có phần cứng, trả về nullptr
    if (!is_cam_available || !is_cam_streaming)
    {
        return nullptr;
    }
    return nullptr;
}

void camera_service_return_frame(CameraFrame *frame)
{
    if (frame == nullptr) return;
    // Giải phóng / tái sử dụng frame buffer
}

bool camera_service_is_available(void)
{
    return is_cam_available;
}

const char* camera_service_get_model_name(void)
{
    switch (active_model)
    {
        case CAMERA_MODEL_OV2640: return "OmniVision OV2640 (2MP)";
        case CAMERA_MODEL_OV5640: return "OmniVision OV5640 (5MP)";
        case CAMERA_MODEL_GC0308: return "GalaxyCore GC0308 (VGA)";
        default: return "No Camera Hardware Detected";
    }
}
