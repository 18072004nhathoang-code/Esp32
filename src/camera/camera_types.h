/**
 * @file camera_types.h
 * @brief Định nghĩa các kiểu dữ liệu, profile camera đa hãng và cấu trúc Frame
 * cho ESP32-S3 Mini OS Camera Subsystem.
 */

#pragma once

#include <Arduino.h>

// Loại nguồn cung cấp Camera
enum CameraSourceType
{
    CAM_SOURCE_NONE = 0,
    CAM_SOURCE_LOCAL_DVP,       // Camera phần cứng trực tiếp qua giao tiếp DVP 8-bit (OV2640 / OV5640)
    CAM_SOURCE_NETWORK_STREAM   // IP Camera qua mạng (ONVIF / RTSP / MJPEG / HTTP Snapshot)
};

// Cảm biến Camera DVP phần cứng cục bộ
enum CameraModel
{
    CAMERA_MODEL_NONE = 0,
    CAMERA_MODEL_OV2640,        // OmniVision OV2640 2MP
    CAMERA_MODEL_OV5640,        // OmniVision OV5640 5MP
    CAMERA_MODEL_GC0308         // GalaxyCore GC0308 VGA
};

// Các profile hỗ trợ của các hãng Camera an ninh phổ biến
enum CameraVendorProfile
{
    CAM_VENDOR_GENERIC_ONVIF = 0, // Chuẩn ONVIF chung (Profile S)
    CAM_VENDOR_HIKVISION,         // Hikvision IP Camera / NVR
    CAM_VENDOR_KBVISION,          // KBVision / Dahua IP Camera
    CAM_VENDOR_EZVIZ,             // Ezviz Cloud / Local RTSP
    CAM_VENDOR_YOOSEE             // Yoosee / SriHome Smart Cam
};

// Giao thức truyền phát hình ảnh qua mạng
enum CameraStreamProtocol
{
    CAM_PROTO_HTTP_SNAPSHOT = 0, // Ảnh tĩnh chụp định kỳ qua HTTP (JPEG)
    CAM_PROTO_MJPEG,             // Luồng Motion JPEG HTTP
    CAM_PROTO_RTSP               // Real Time Streaming Protocol (RTSP H.264/H.265)
};

// Định dạng điểm ảnh đầu ra
enum CameraPixelFormat
{
    CAM_PIXFORMAT_RGB565 = 0,
    CAM_PIXFORMAT_JPEG,
    CAM_PIXFORMAT_GRAYSCALE
};

// Độ phân giải khung hình
enum CameraFrameSize
{
    CAM_FRAMESIZE_QVGA_320x240 = 0,
    CAM_FRAMESIZE_HVGA_480x320,
    CAM_FRAMESIZE_VGA_640x480,
    CAM_FRAMESIZE_HD_1280x720
};

// Trạng thái khả dụng thực tế của từng tính năng/giao thức
enum CameraFeatureStatus
{
    CAM_STATUS_NOT_IMPLEMENTED = 0, // Chưa triển khai (hoặc chỉ mới dựng scaffold)
    CAM_STATUS_NOT_DETECTED,        // Phần cứng chưa được phát hiện
    CAM_STATUS_READY,               // Sẵn sàng hoạt động hoàn chỉnh
    CAM_STATUS_PARTIAL_FALLBACK,    // Triển khai một phần / chuyển sang adapter dự phòng
    CAM_STATUS_ERROR                // Lỗi kết nối hoặc xử lý
};

inline const char* camera_feature_status_to_string(CameraFeatureStatus st)
{
    switch (st)
    {
        case CAM_STATUS_READY: return "READY";
        case CAM_STATUS_NOT_DETECTED: return "NOT_DETECTED";
        case CAM_STATUS_PARTIAL_FALLBACK: return "PARTIAL / FALLBACK";
        case CAM_STATUS_ERROR: return "ERROR";
        case CAM_STATUS_NOT_IMPLEMENTED:
        default: return "NOT_IMPLEMENTED";
    }
}

// Máy trạng thái runtime của Camera Service
enum CameraRuntimeState
{
    CAM_STATE_NOT_CONFIGURED = 0,
    CAM_STATE_CONNECTING,
    CAM_STATE_CONNECTED,
    CAM_STATE_ERROR,
    CAM_STATE_STOPPED
};

inline const char* camera_runtime_state_to_string(CameraRuntimeState st)
{
    switch (st)
    {
        case CAM_STATE_CONNECTING: return "CONNECTING";
        case CAM_STATE_CONNECTED:  return "CONNECTED";
        case CAM_STATE_ERROR:      return "ERROR";
        case CAM_STATE_STOPPED:    return "STOPPED";
        case CAM_STATE_NOT_CONFIGURED:
        default:                   return "NOT_CONFIGURED";
    }
}

// Cấu trúc khung hình (Frame Buffer)
struct CameraFrame
{
    uint8_t *buf;
    size_t len;
    size_t width;
    size_t height;
    CameraPixelFormat format;
    uint32_t timestamp_ms;
    uint32_t frame_id; // Sequence ID phân biệt frame mới
};

// Cấu hình Camera DVP cục bộ
struct LocalCameraConfig
{
    CameraModel model;
    CameraFrameSize frame_size;
    CameraPixelFormat pixel_format;
    uint8_t jpeg_quality; // 0 - 63
    uint8_t fb_count;     // Số buffer PSRAM (1 hoặc 2)

    // Sơ đồ chân giao tiếp DVP 8-bit
    int8_t pin_pwdn;
    int8_t pin_reset;
    int8_t pin_xclk;
    int8_t pin_siod;      // SCCB SDA
    int8_t pin_sioc;      // SCCB SCL
    int8_t pin_d7;
    int8_t pin_d6;
    int8_t pin_d5;
    int8_t pin_d4;
    int8_t pin_d3;
    int8_t pin_d2;
    int8_t pin_d1;
    int8_t pin_d0;
    int8_t pin_vsync;
    int8_t pin_href;
    int8_t pin_pclk;

    uint32_t xclk_freq_hz;
};

// Thông số cấu hình IP Camera qua mạng
struct NetworkCameraProfile
{
    CameraVendorProfile vendor;
    CameraStreamProtocol protocol;
    char name[32];
    char ip[48];
    uint16_t http_port;
    uint16_t rtsp_port;
    uint16_t onvif_port;
    char username[32];
    char password[32];
    uint8_t channel;
    char custom_url[128];
};
