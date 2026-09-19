/**
 * @file wifi_config.h
 * @brief Cấu hình mạng WiFi cho hệ điều hành ESP32-S3 Mini OS
 */

#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif

#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif

// Thời gian chờ kết nối tối đa (mili-giây)
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 15000
#endif

// Bật tự động kết nối lại khi mất sóng (1 = Bật, 0 = Tắt)
#ifndef WIFI_AUTO_RECONNECT
#define WIFI_AUTO_RECONNECT 1
#endif

// Tên phân vùng NVS lưu trữ thông tin mạng
#define WIFI_PREFS_NAMESPACE "minios_wifi"
#define WIFI_PREFS_KEY_SSID  "ssid"
#define WIFI_PREFS_KEY_PASS  "pass"
#define WIFI_PREFS_KEY_CHK   "chk"
#define WIFI_PREFS_KEY_BAK_SSID "b_ssid"
#define WIFI_PREFS_KEY_BAK_PASS "b_pass"
#define WIFI_PREFS_KEY_BAK_CHK  "b_chk"
