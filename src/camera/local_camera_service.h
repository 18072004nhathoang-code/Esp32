/**
 * @file local_camera_service.h
 * @brief Quản lý và thu nhận khung hình từ cảm biến Camera DVP cục bộ (OV2640 / OV5640)
 */

#pragma once

#include "camera_types.h"

class LocalCameraService
{
public:
    LocalCameraService();
    ~LocalCameraService();

    bool init(const LocalCameraConfig &config);
    bool start();
    void stop();
    bool isAvailable() const;
    bool isStreaming() const;
    CameraFrame* getFrame(uint32_t timeout_ms = 1000);
    void returnFrame(CameraFrame *frame);
    const char* getSensorName() const;

private:
    bool _initialized;
    bool _streaming;
    CameraModel _model;
    LocalCameraConfig _config;
};

extern LocalCameraService g_local_camera;
