/**
 * @file camera_types.h
 * @brief Định nghĩa các kiểu dữ liệu, cấu hình và cấu trúc Frame cho hệ thống Camera / RTSP
 */

#pragma once

#include <Arduino.h>

// Các dòng cảm biến Camera tương thích ESP32-S3
enum CameraModel
{
    CAMERA_MODEL_NONE = 0,
    CAMERA_MODEL_OV2640,
    CAMERA_MODEL_OV5640,
    CAMERA_MODEL_GC0308
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

// Cấu trúc khung hình (Frame Buffer)
struct CameraFrame
{
    uint8_t *buf;
    size_t len;
    size_t width;
    size_t height;
    CameraPixelFormat format;
    uint32_t timestamp_ms;
};

// Cấu hình chân phần cứng và thông số DVP Camera
struct CameraConfig
{
    CameraModel model;
    CameraFrameSize frame_size;
    CameraPixelFormat pixel_format;
    uint8_t jpeg_quality; // 0 - 63 (chất lượng cao nhất: số nhỏ hơn)
    uint8_t fb_count;     // Số lượng frame buffer trong PSRAM (1 hoặc 2)

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

    uint32_t xclk_freq_hz; // Tần số clock XCLK (10MHz - 20MHz)
};
