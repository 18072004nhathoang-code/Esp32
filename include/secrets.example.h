/**
 * @file secrets.example.h
 * @brief Template cấu hình thông tin bảo mật cho ESP32-S3 Mini OS
 * HƯỚNG DẪN: Sao chép file này thành include/secrets.h và điền thông tin thực tế.
 * File include/secrets.h đã được thêm vào .gitignore để không bị lộ trên Git.
 */

#pragma once

// Cấu hình mạng WiFi mặc định (nếu không dùng NVS)
#define DEFAULT_WIFI_SSID       ""
#define DEFAULT_WIFI_PASS       ""

// Google Maps Static API Key (để tải ảnh vệ tinh / roadmap chất lượng cao)
// Lấy key tại: https://console.cloud.google.com/
#define GOOGLE_MAPS_STATIC_API_KEY ""

// Secure voice gateway contract. Point these at the supplied backend.
// Query may also return allowlisted music actions and grounded source links.
#define AI_VOICE_ENDPOINT       "https://assistant.example.com/v1/query"
#define AI_VOICE_TTS_ENDPOINT   "https://assistant.example.com/v1/tts"
#define AI_VOICE_BEARER_TOKEN   ""
#define AI_VOICE_CA_CERT        ""

// Internet music is resolved locally by source ID. The backend must have the
// same IDs in MUSIC_SOURCES_JSON. Gemini never supplies a URL to the device.
// Keep this on one line and use HTTPS sources only (maximum 8 entries).
#define AI_MUSIC_STREAM_SOURCES_JSON "[]"
// PEM trust anchor (or concatenated PEM roots) for the configured streams.
#define AI_MUSIC_STREAM_CA_CERT ""

// PEM CA certificate used to verify HTTPS IP-camera endpoints. Keep empty until
// a trusted CA for the camera has been provisioned; verified TLS then fails closed.
#define CAMERA_TLS_CA_CERT      ""
