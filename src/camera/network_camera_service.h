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
    bool stop(uint32_t timeout_ms = 2000);
    bool isConnected() const;
    NetworkCameraProfile getActiveProfile() const;

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

    // Tải ảnh trực tiếp qua HTTP Snapshot (nhận pointer & capacity theo tham chiếu để realloc an toàn)
    int fetchHttpSnapshot(uint8_t *&out_buf, size_t &current_cap);

    // Lấy trạng thái runtime thật (NOT_CONFIGURED, CONNECTING, CONNECTED, PASSWORD_REQUIRED, ERROR, STOPPED)
    CameraRuntimeState getRuntimeState() const;
    CameraTransportSecurity getTransportSecurity() const;
    CameraFailureReason getFailureReason() const;
    uint32_t getSessionId() const;

    // Lưu & Nạp cấu hình Camera từ NVS Flash (không lưu password dạng plaintext)
    bool saveProfileToNVS();
    bool loadProfileFromNVS();

    static const char* getVendorName(CameraVendorProfile vendor);
    static void urlEncode(const char *src, char *dst, size_t dst_len);
    static void sanitizeUrl(const char *src, char *dst, size_t dst_len);
    static bool parseJpegDimensions(const uint8_t *buf, size_t len, size_t &width, size_t &height);

private:
    bool _configured;
    bool _connected;
    volatile bool _running;
    CameraRuntimeState _runtime_state;
    volatile CameraTransportSecurity _transport_security;
    volatile CameraFailureReason _failure_reason;
    uint32_t _frame_sequence;
    uint32_t _session_id;
    uint32_t _worker_session_id;
    NetworkCameraProfile _profile;
    mutable SemaphoreHandle_t _config_mutex;
    SemaphoreHandle_t _worker_exit_sem;

    char _onvif_snapshot_url[192];
    char _onvif_stream_url[192];
    bool _onvif_probed;

    // Ping-pong / Double Buffering với dung lượng độc lập cho từng buffer
    uint8_t *_buf_front;
    uint8_t *_buf_back;
    size_t _front_capacity;
    size_t _back_capacity;
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
    int fetchHttpSnapshotForProfile(uint8_t *&out_buf, size_t &current_cap,
                                    const NetworkCameraProfile &request_profile);
    bool ensureSynchronizationPrimitives();
    void setTransportSecurity(CameraTransportSecurity security);
    bool snapshotState(NetworkCameraProfile &profile, bool &configured,
                       CameraRuntimeState &state) const;
};

extern NetworkCameraService g_network_camera;

