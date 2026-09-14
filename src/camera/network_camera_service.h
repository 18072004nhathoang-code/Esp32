#pragma once

#include "camera_types.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

class NetworkCameraService
{
public:
    NetworkCameraService();
    ~NetworkCameraService();

    bool configure(const NetworkCameraProfile &profile);
    bool start();
    void stop();
    bool isConnected() const;
    const NetworkCameraProfile& getActiveProfile() const;

    // Lấy trạng thái khả năng thực tế của từng tính năng
    CameraFeatureStatus getSnapshotStatus() const;
    CameraFeatureStatus getMjpegStatus() const;
    CameraFeatureStatus getRtspStatus() const;
    CameraFeatureStatus getOnvifStatus() const;

    // Thu nhận khung hình qua CameraFrame (Non-blocking cho LVGL)
    CameraFrame* getFrame(uint32_t timeout_ms = 1000);
    void returnFrame(CameraFrame *frame);

    // URL helpers với URL-encode và bảo mật (che mật khẩu khi log)
    void buildStreamUrl(char *out_url, size_t max_len, bool mask_credential = false) const;
    void buildSnapshotUrl(char *out_url, size_t max_len) const;

    // ONVIF Client Minimal Protocol (WS-Discovery / GetCapabilities / GetSnapshotUri)
    bool onvifProbeCapabilities(char *out_service_url, size_t max_len);
    bool onvifGetProfiles(char *out_profile_token, size_t max_len);
    bool onvifGetSnapshotUri(const char *profile_token, char *out_uri, size_t max_len);
    bool onvifGetStreamUri(const char *profile_token, char *out_uri, size_t max_len);

    // Tải ảnh trực tiếp qua HTTP Snapshot (có timeout, allocation check)
    int fetchHttpSnapshot(uint8_t *out_buf, size_t max_size);

    static const char* getVendorName(CameraVendorProfile vendor);
    static void urlEncode(const char *src, char *dst, size_t dst_len);

private:
    bool _configured;
    bool _connected;
    bool _running;
    NetworkCameraProfile _profile;

    char _onvif_snapshot_url[192];
    char _onvif_stream_url[192];
    bool _onvif_probed;

    CameraFrame _current_frame;
    uint8_t *_snapshot_jpeg_buf;
    size_t _snapshot_buf_size;
    SemaphoreHandle_t _frame_mutex;
    TaskHandle_t _worker_task_handle;

    CameraFeatureStatus _snapshot_status;
    CameraFeatureStatus _mjpeg_status;
    CameraFeatureStatus _rtsp_status;
    CameraFeatureStatus _onvif_status;

    static void workerTaskEntry(void *param);
    void workerTask();
};

extern NetworkCameraService g_network_camera;

