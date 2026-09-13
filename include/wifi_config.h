/**
 * @file wifi_config.h
 * @brief Cấu hình mạng WiFi cho hệ điều hành ESP32-S3 Mini OS
 */

#pragma once

// Nếu bạn muốn bo mạch tự động kết nối WiFi nhà bạn ngay khi bật nguồn,
// hãy điền tên mạng và mật khẩu vào đây. Nếu để trống, bạn có thể quét và
// nhập mật khẩu trực tiếp bằng bàn phím cảm ứng trên màn hình 2.8".
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASS ""

// Thời gian chờ kết nối tối đa (mili-giây)
#define WIFI_CONNECT_TIMEOUT_MS 15000

// Bật tự động kết nối lại khi mất sóng
#define WIFI_AUTO_RECONNECT 1

// Tên phân vùng NVS lưu trữ thông tin mạng
#define WIFI_PREFS_NAMESPACE "minios_wifi"
#define WIFI_PREFS_KEY_SSID  "ssid"
#define WIFI_PREFS_KEY_PASS  "pass"
