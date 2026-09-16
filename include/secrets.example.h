/**
 * @file secrets.example.h
 * @brief Template cấu hình thông tin bảo mật cho ESP32-S3 Mini OS
 * HƯỚNG DẪN: Sao chép file này thành include/secrets.h và điền thông tin thực tế.
 * File include/secrets.h đã được thêm vào .gitignore để không bị lộ trên Git.
 */

#pragma once

// Cấu hình mạng WiFi mặc định (nếu không dùng NVS)
#define DEFAULT_WIFI_SSID       "Your_WiFi_SSID"
#define DEFAULT_WIFI_PASS       "Your_WiFi_Password"

// Google Maps Static API Key (để tải ảnh vệ tinh / roadmap chất lượng cao)
// Lấy key tại: https://console.cloud.google.com/
#define GOOGLE_MAPS_STATIC_API_KEY ""

// Secure voice gateway contract:
// AI_VOICE_ENDPOINT accepts POST audio/wav and returns JSON containing
// "transcript" and "reply". The TTS endpoint returns mono PCM16 16kHz WAV.
#define AI_VOICE_ENDPOINT       ""
#define AI_VOICE_TTS_ENDPOINT   ""
#define AI_VOICE_BEARER_TOKEN   ""
#define AI_VOICE_CA_CERT        ""

// PEM CA certificate used to verify HTTPS IP-camera endpoints. Keep empty until
// a trusted CA for the camera has been provisioned; verified TLS then fails closed.
#define CAMERA_TLS_CA_CERT      ""
