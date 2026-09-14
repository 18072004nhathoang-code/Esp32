/**
 * @file camera_service.cpp
 * @brief Triển khai tầng dịch vụ thống nhất quản lý Camera cho UI (Vendor-Agnostic Facade)
 */

#include "camera_service.h"

static CameraSourceType active_source = CAM_SOURCE_LOCAL_DVP;
static char status_text_buffer[64] = "Chưa kết nối Camera";

bool camera_service_init(void)
{
    Serial.println("[CAMERA] Khởi tạo hệ thống quản lý Camera đa nguồn...");

    // Cấu hình thử Camera DVP cục bộ
    LocalCameraConfig local_cfg;
    memset(&local_cfg, 0, sizeof(local_cfg));
    g_local_camera.init(local_cfg);

    if (g_local_camera.isAvailable())
    {
        active_source = CAM_SOURCE_LOCAL_DVP;
        snprintf(status_text_buffer, sizeof(status_text_buffer), "DVP: %s", g_local_camera.getSensorName());
    }
    else
    {
        active_source = CAM_SOURCE_NETWORK_STREAM;
        snprintf(status_text_buffer, sizeof(status_text_buffer), "Mạng: Sẵn sàng kết nối IP Cam");
    }

    return true;
}

void camera_service_set_source(CameraSourceType source)
{
    active_source = source;
}

CameraSourceType camera_service_get_source(void)
{
    return active_source;
}

bool camera_service_configure_network(const NetworkCameraProfile &profile)
{
    bool ok = g_network_camera.configure(profile);
    if (ok)
    {
        snprintf(status_text_buffer, sizeof(status_text_buffer), "%s (%s)",
                 NetworkCameraService::getVendorName(profile.vendor), profile.ip);
    }
    return ok;
}

bool camera_service_start(void)
{
    if (active_source == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.start();
    }
    else if (active_source == CAM_SOURCE_NETWORK_STREAM)
    {
        return g_network_camera.start();
    }
    return false;
}

void camera_service_stop(void)
{
    if (active_source == CAM_SOURCE_LOCAL_DVP)
    {
        g_local_camera.stop();
    }
    else if (active_source == CAM_SOURCE_NETWORK_STREAM)
    {
        g_network_camera.stop();
    }
}

CameraFrame* camera_service_get_frame(uint32_t timeout_ms)
{
    if (active_source == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.getFrame(timeout_ms);
    }
    return nullptr;
}

void camera_service_return_frame(CameraFrame *frame)
{
    if (active_source == CAM_SOURCE_LOCAL_DVP)
    {
        g_local_camera.returnFrame(frame);
    }
}

bool camera_service_is_available(void)
{
    if (active_source == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.isAvailable();
    }
    else if (active_source == CAM_SOURCE_NETWORK_STREAM)
    {
        return g_network_camera.isConnected();
    }
    return false;
}

const char* camera_service_get_status_text(void)
{
    return status_text_buffer;
}

const char* camera_service_get_model_name(void)
{
    if (active_source == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.getSensorName();
    }
    return "Network IP Camera";
}
