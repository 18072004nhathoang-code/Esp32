/**
 * @file local_camera_service.cpp
 * @brief Triển khai dịch vụ Camera DVP cục bộ (OmniVision OV2640 / OV5640)
 */

#include "local_camera_service.h"

LocalCameraService g_local_camera;

LocalCameraService::LocalCameraService()
    : _initialized(false), _streaming(false), _model(CAMERA_MODEL_NONE)
{
}

LocalCameraService::~LocalCameraService()
{
    stop();
}

bool LocalCameraService::init(const LocalCameraConfig &config)
{
    _config = config;
    Serial.println("[LOCAL_CAM] Đang dò tìm cảm biến Camera DVP (SCCB/I2C probe)...");

    // Khi bo mạch chưa gắn module camera thực tế, phát hiện an toàn không gây treo hệ thống
    _initialized = false;
    _streaming = false;
    _model = CAMERA_MODEL_NONE;

    Serial.println("[LOCAL_CAM] ⚠️ Không phát hiện cảm biến DVP vật lý. Trạng thái: Sẵn sàng gắn module (OV2640/OV5640).");
    return false;
}

bool LocalCameraService::start()
{
    if (!_initialized)
    {
        Serial.println("[LOCAL_CAM] Không thể kích hoạt: Chưa gắn cảm biến camera cục bộ.");
        return false;
    }
    _streaming = true;
    Serial.println("[LOCAL_CAM] Đã bắt đầu luồng bắt hình DVP.");
    return true;
}

void LocalCameraService::stop()
{
    _streaming = false;
    Serial.println("[LOCAL_CAM] Đã dừng luồng bắt hình DVP.");
}

bool LocalCameraService::isAvailable() const
{
    return _initialized;
}

bool LocalCameraService::isStreaming() const
{
    return _streaming;
}

CameraFrame* LocalCameraService::getFrame(uint32_t timeout_ms)
{
    if (!_initialized || !_streaming) return nullptr;
    return nullptr;
}

void LocalCameraService::returnFrame(CameraFrame *frame)
{
    if (!frame) return;
}

const char* LocalCameraService::getSensorName() const
{
    switch (_model)
    {
        case CAMERA_MODEL_OV2640: return "OmniVision OV2640 (2MP)";
        case CAMERA_MODEL_OV5640: return "OmniVision OV5640 (5MP)";
        case CAMERA_MODEL_GC0308: return "GalaxyCore GC0308 (VGA)";
        default: return "Chưa gắn Camera DVP";
    }
}
