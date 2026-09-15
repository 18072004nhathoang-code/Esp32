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

    // Thu nhận khung hình qua CameraFrame (Double-buffer bảo vệ, non-blocking / timeout cho UI)
    CameraFrame* getFrame(uint32_t timeout_ms = 200);
    void returnFrame(CameraFrame *frame);

    // URL helpers với URL-encode và bảo mật (che mật khẩu khi log)
    void buildStreamUrl(char *out_url, size_t max_len, bool mask_credential = false) const;
    void buildSnapshotUrl(char *out_url, size_t max_len) const;

    // ONVIF Client Protocol (Thực tế, không return fake success)
    bool onvifProbeCapabilities(char *out_service_url, size_t max_len);
    bool onvifGetProfiles(char *out_profile_token, size_t max_len);
    bool onvifGetSnapshotUri(const char *profile_token, char *out_uri, size_t max_len);
    bool onvifGetStreamUri(const char *profile_token, char *out_uri, size_t max_len);

    // Tải ảnh trực tiếp qua HTTP Snapshot (hỗ trợ Content-Length và Chunked/Stream)
    int fetchHttpSnapshot(uint8_t *out_buf, size_t max_size);

    static const char* getVendorName(CameraVendorProfile vendor);
    static void urlEncode(const char *src, char *dst, size_t dst_len);
    static void sanitizeUrl(const char *src, char *dst, size_t dst_len);
    static bool parseJpegDimensions(const uint8_t *buf, size_t len, size_t &width, size_t &height);

private:
    bool _configured;
    bool _connected;
    bool _running;
    NetworkCameraProfile _profile;

    char _onvif_snapshot_url[192];
    char _onvif_stream_url[192];
    bool _onvif_probed;

    // Ping-pong / Double Buffering để worker không bao giờ overwrite khi consumer đang đọc
    uint8_t *_buf_front;
    uint8_t *_buf_back;
    size_t _buf_capacity;
    CameraFrame _frame_front;
    CameraFrame _frame_back;
    bool _front_in_use;
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

