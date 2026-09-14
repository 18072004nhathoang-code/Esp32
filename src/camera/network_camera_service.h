/**
 * @file network_camera_service.h
 * @brief Dịch vụ kết nối và thu nhận luồng IP Camera qua mạng (ONVIF, HTTP Snapshot, MJPEG, RTSP)
 * Hỗ trợ các profile định sẵn cho camera Hikvision, KBVision/Dahua, Ezviz, Yoosee.
 */

#pragma once

#include "camera_types.h"

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

    // Tạo URL luồng video hoặc ảnh chụp theo chuẩn từng hãng
    void buildStreamUrl(char *out_url, size_t max_len) const;
    void buildSnapshotUrl(char *out_url, size_t max_len) const;

    // Tải ảnh chụp tĩnh qua HTTP Snapshot
    int fetchHttpSnapshot(uint8_t *out_buf, size_t max_size);

    static const char* getVendorName(CameraVendorProfile vendor);

private:
    bool _configured;
    bool _connected;
    NetworkCameraProfile _profile;
};

extern NetworkCameraService g_network_camera;
