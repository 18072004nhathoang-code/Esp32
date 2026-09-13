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

// Google Gemini API Key / OpenAI API Key cho trợ lý giọng nói AI
#define GEMINI_API_KEY          ""
#define OPENAI_API_KEY          ""
