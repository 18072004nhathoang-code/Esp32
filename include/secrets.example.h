/**
 * @file secrets.example.h
 * @brief Template cấu hình thông tin bảo mật cho ESP32-S3 Mini OS.
 * Sao chép file này thành include/secrets.h và điền thông tin thực tế.
 * include/secrets.h đã được thêm vào .gitignore.
 */

#pragma once

#define DEFAULT_WIFI_SSID       ""
#define DEFAULT_WIFI_PASS       ""

#define GOOGLE_MAPS_STATIC_API_KEY ""

// Xiaozhi is the default provider. The bundled trust anchors are used whenever
// the deployment-specific values below are empty.
#define AI_VOICE_PROVIDER_XIAOZHI 1
#define XIAOZHI_OTA_ENDPOINT     "https://api.tenclass.net/xiaozhi/ota/"
#define XIAOZHI_OTA_CA_CERT      ""
#define XIAOZHI_WSS_CA_CERT      ""

// Legacy secure voice gateway contract (disabled when Xiaozhi is selected).
// Provider keys stay on the backend; firmware receives only a device token.
#define AI_VOICE_ENDPOINT       "https://assistant.example.com/v1/query"
#define AI_VOICE_TTS_ENDPOINT   "https://assistant.example.com/v1/tts"
#define AI_VOICE_BEARER_TOKEN   ""
#define AI_VOICE_CA_CERT        ""

// Internet music is resolved by source ID. Do not put resolved media URLs here.
#define AI_MUSIC_STREAM_SOURCES_JSON "[]"

// Local/LAN YouTube proxy. Firmware appends ?q=... (or &q=...).
#define YOUTUBE_STREAM_ENDPOINT ""
#define YOUTUBE_PROXY_USER      ""
#define YOUTUBE_PROXY_PASSWORD  ""
#define AI_MUSIC_STREAM_CA_CERT ""

// HTTPS snapshot camera trust anchor. Empty means verified TLS fails closed.
#define CAMERA_TLS_CA_CERT      ""
