/**
 * @file camera_service.cpp
 * @brief Triển khai tầng dịch vụ thống nhất quản lý Camera cho UI (Vendor-Agnostic Facade)
 */

#include "camera_service.h"
#include "board_config.h"
#include <atomic>

namespace
{
std::atomic<CameraSourceType> active_source{CAM_SOURCE_LOCAL_DVP};

CameraSourceType current_source()
{
    return active_source.load(std::memory_order_acquire);
}
}

bool camera_service_init(void)
{
    Serial.println("[CAMERA] Khởi tạo hệ thống quản lý Camera đa nguồn...");

#if defined(BOARD_HAS_LOCAL_CAMERA) && (BOARD_HAS_LOCAL_CAMERA == 1)
    // Cấu hình thử Camera DVP cục bộ
    LocalCameraConfig local_cfg;
    memset(&local_cfg, 0, sizeof(local_cfg));
    g_local_camera.init(local_cfg);

    if (g_local_camera.isAvailable())
    {
        active_source.store(CAM_SOURCE_LOCAL_DVP, std::memory_order_release);
    }
    else
    {
        active_source.store(CAM_SOURCE_NETWORK_STREAM, std::memory_order_release);
    }
#else
    active_source.store(CAM_SOURCE_NETWORK_STREAM, std::memory_order_release);
    g_network_camera.loadProfileFromNVS();
    Serial.println("[CAMERA] Board không có local DVP camera -> Chuyển hoàn toàn sang Network Camera (IP Cam).");
#endif

    return true;
}

void camera_service_set_source(CameraSourceType source)
{
    active_source.store(source, std::memory_order_release);
}

CameraSourceType camera_service_get_source(void)
{
    return current_source();
}

bool camera_service_configure_network(const NetworkCameraProfile &profile)
{
    bool ok = g_network_camera.configure(profile);
    return ok;
}

bool camera_service_save_network_profile(void)
{
    return g_network_camera.saveProfileToNVS();
}

NetworkCameraProfile camera_service_get_network_profile(void)
{
    return g_network_camera.getActiveProfile();
}

CameraRuntimeState camera_service_get_runtime_state(void)
{
    if (current_source() == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.isAvailable() ? CAM_STATE_RUNNING : CAM_STATE_NOT_CONFIGURED;
    }
    return g_network_camera.getRuntimeState();
}

uint32_t camera_service_get_session_id(void)
{
    return current_source() == CAM_SOURCE_NETWORK_STREAM ? g_network_camera.getSessionId() : 0;
}

CameraFailureReason camera_service_get_failure_reason(void)
{
    return current_source() == CAM_SOURCE_NETWORK_STREAM
        ? g_network_camera.getFailureReason() : CAM_FAILURE_NONE;
}

CameraTransportSecurity camera_service_get_transport_security(void)
{
    return current_source() == CAM_SOURCE_NETWORK_STREAM
        ? g_network_camera.getTransportSecurity() : CAM_TRANSPORT_NONE;
}

bool camera_service_is_connected(void)
{
    if (current_source() == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.isAvailable();
    }
    return g_network_camera.isConnected();
}

bool camera_service_start(void)
{
    const CameraSourceType source = current_source();
    if (source == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.start();
    }
    else if (source == CAM_SOURCE_NETWORK_STREAM)
    {
        return g_network_camera.start();
    }
    return false;
}

bool camera_service_stop(uint32_t timeout_ms)
{
    const CameraSourceType source = current_source();
    if (source == CAM_SOURCE_LOCAL_DVP)
    {
        g_local_camera.stop();
        return true;
    }
    else if (source == CAM_SOURCE_NETWORK_STREAM)
    {
        return g_network_camera.stop(timeout_ms);
    }
    return true;
}

CameraFrame* camera_service_get_frame(uint32_t timeout_ms)
{
    const CameraSourceType source = current_source();
    if (source == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.getFrame(timeout_ms);
    }
    else if (source == CAM_SOURCE_NETWORK_STREAM)
    {
        return g_network_camera.getFrame(timeout_ms);
    }
    return nullptr;
}

void camera_service_return_frame(CameraFrame *frame)
{
    if (!frame) return;
    // A source switch may happen after getFrame(). Return the lease to the
    // service that produced it, not to whichever source is selected now.
    if (frame->source == CAM_SOURCE_LOCAL_DVP)
    {
        g_local_camera.returnFrame(frame);
    }
    else if (frame->source == CAM_SOURCE_NETWORK_STREAM)
    {
        g_network_camera.returnFrame(frame);
    }
}

bool camera_service_is_available(void)
{
    const CameraSourceType source = current_source();
    if (source == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.isAvailable();
    }
    else if (source == CAM_SOURCE_NETWORK_STREAM)
    {
        return g_network_camera.isConnected();
    }
    return false;
}

const char* camera_service_get_status_text(void)
{
    if (current_source() == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.isAvailable() ? "CONNECTED" : "NOT_DETECTED";
    }

    CameraRuntimeState st = g_network_camera.getRuntimeState();
    switch (st)
    {
        case CAM_STATE_STARTING:          return "STARTING";
        case CAM_STATE_RUNNING:
            if (g_network_camera.getTransportSecurity() == CAM_TRANSPORT_HTTP_PLAINTEXT)
                return "WARNING: HTTP PLAINTEXT";
            if (g_network_camera.getTransportSecurity() == CAM_TRANSPORT_HTTPS_UNVERIFIED)
                return "HTTPS: CERT UNVERIFIED";
            return "RUNNING";
        case CAM_STATE_STOPPING:          return "STOPPING";
        case CAM_STATE_PASSWORD_REQUIRED: return "PASSWORD_REQUIRED";
        case CAM_STATE_ERROR:             return "ERROR";
        case CAM_STATE_STOPPED:           return "STOPPED";
        case CAM_STATE_NOT_CONFIGURED:
        default:                          return "NEEDS_USER_INPUT";
    }
}

const char* camera_service_get_model_name(void)
{
    if (current_source() == CAM_SOURCE_LOCAL_DVP)
    {
        return g_local_camera.getSensorName();
    }
    return "Network IP Camera";
}

CameraFeatureStatus camera_service_get_snapshot_status(void)
{
    return g_network_camera.getSnapshotStatus();
}

CameraFeatureStatus camera_service_get_mjpeg_status(void)
{
    return g_network_camera.getMjpegStatus();
}

CameraFeatureStatus camera_service_get_rtsp_status(void)
{
    return g_network_camera.getRtspStatus();
}

CameraFeatureStatus camera_service_get_onvif_status(void)
{
    return g_network_camera.getOnvifStatus();
}

CameraFeatureStatus camera_service_get_local_dvp_status(void)
{
    return g_local_camera.isAvailable() ? CAM_STATUS_READY : CAM_STATUS_NOT_DETECTED;
}
