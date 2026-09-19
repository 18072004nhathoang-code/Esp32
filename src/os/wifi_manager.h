/**
 * @file wifi_manager.h
 * @brief Dịch vụ quản lý WiFi chạy nền trên Core 0 cho ESP32-S3 Mini OS
 */

#pragma once

#include <Arduino.h>
#include <vector>
#include "wifi_scan_coordinator.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif

#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif

enum WiFiState {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_SCANNING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_DISCONNECTING,
    WIFI_STATE_FORGETTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED
};

enum WiFiControlStatus {
    WIFI_CONTROL_NONE = 0,
    WIFI_CONTROL_REQUESTED,
    WIFI_CONTROL_APPLIED,
    WIFI_CONTROL_FAILED
};

struct WiFiNetworkInfo {
    char ssid[33];
    int32_t rssi;
    bool is_encrypted;
    uint8_t channel;
};

static constexpr size_t WIFI_SCAN_MAX_RESULTS = 32;

struct WiFiScanSnapshot {
    uint32_t request_id;
    uint32_t revision;
    uint32_t result_revision;
    uint32_t queued_at_ms;
    uint32_t started_at_ms;
    WifiScanPhase state;
    int32_t driver_error;
    char error[96];
    size_t result_count;
    WiFiNetworkInfo results[WIFI_SCAN_MAX_RESULTS];
};

/**
 * @brief Khởi tạo hệ thống WiFi, đọc thông tin mạng đã lưu trong NVS Flash
 */
bool wifi_manager_init(void);

/**
 * @brief Bắt đầu quét mạng WiFi xung quanh (bất đồng bộ)
 */
bool wifi_manager_scan_async(void);

/** Cancel the current scan request without treating scanDelete() as a driver cancel. */
bool wifi_manager_cancel_scan(void);

/** Atomic scan state/results snapshot. */
WiFiScanSnapshot wifi_manager_get_scan_snapshot(void);

/**
 * @brief Kiểm tra xem quá trình quét mạng đã hoàn thành chưa
 */
bool wifi_manager_is_scan_done(void);

/** @brief Lỗi gần nhất từ scan/connect/NVS; không chứa mật khẩu. */
String wifi_manager_get_last_error(void);

/** Connection error is intentionally independent from scan errors. */
String wifi_manager_get_connection_error(void);

/**
 * @brief Lấy danh sách các mạng WiFi vừa quét được
 */
std::vector<WiFiNetworkInfo> wifi_manager_get_scan_results(void);

/**
 * @brief Bắt đầu kết nối tới một mạng WiFi
 * @param ssid Tên mạng
 * @param pass Mật khẩu
 * @param save_to_nvs Đánh dấu lưu vào Flash NVS khi kết nối thành công (mặc định true khi người dùng cấu hình)
 * @return true nếu lệnh kết nối được tiếp nhận
 */
bool wifi_manager_connect(const char *ssid, const char *pass, bool save_to_nvs = true);

/**
 * @brief Ngắt kết nối WiFi
 */
void wifi_manager_disconnect(void);

/**
 * @brief Lấy trạng thái hoạt động hiện tại của WiFi
 */
WiFiState wifi_manager_get_state(void);

/**
 * @brief Kiểm tra nhanh xem đã có kết nối Internet/WiFi chưa
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Lấy địa chỉ IP cục bộ dưới dạng chuỗi (ví dụ: "192.168.1.45")
 */
String wifi_manager_get_ip(void);

/**
 * @brief Lấy tên mạng WiFi đang kết nối
 */
String wifi_manager_get_ssid(void);

/**
 * @brief Lấy độ mạnh tín hiệu sóng (RSSI dBm)
 */
int8_t wifi_manager_get_rssi(void);

/**
 * @brief Kiểm tra xem trong Flash NVS đã có cấu hình WiFi được lưu trước đó chưa
 */
bool wifi_manager_has_saved_credentials(void);

/**
 * @brief Trạng thái lưu trữ NVS của thông tin WiFi
 */
enum WifiSaveStatus : uint8_t
{
    WIFI_SAVE_NONE = 0,
    WIFI_SAVE_PENDING,
    WIFI_SAVED,
    WIFI_SAVE_FAILED
};

/**
 * @brief Lấy trạng thái lưu thông tin mạng hiện tại (Thread-Safe snapshot, không đọc NVS)
 */
WifiSaveStatus wifi_manager_get_save_status(void);

/**
 * @brief Kiểm tra xem mạng WiFi hiện tại đã được lưu thành công vào NVS chưa (Thread-Safe snapshot)
 */
bool wifi_manager_is_credentials_saved(void);


/**
 * @brief Lưu thông tin WiFi vào bộ nhớ Flash NVS (Preferences)
 */
bool wifi_manager_save_credentials(const char *ssid, const char *pass);

/**
 * @brief Đọc thông tin WiFi đã lưu từ NVS Flash
 */
bool wifi_manager_load_credentials(String &ssid, String &pass);

/**
 * @brief Xóa thông tin WiFi đã lưu trong NVS Flash
 */
bool wifi_manager_clear_credentials(void);

/**
 * @brief Quên mạng hiện tại (Xóa NVS, xóa mục tiêu kết nối và chặn tự động reconnect)
 */
bool wifi_manager_forget_network(void);
WiFiControlStatus wifi_manager_get_control_status(void);

/**
 * @brief Bật hoặc tắt tính năng tự động kết nối lại khi mất sóng
 */
void wifi_manager_set_auto_reconnect(bool enable);

/**
 * @brief Kiểm tra trạng thái cấu hình tự động kết nối lại
 */
bool wifi_manager_is_auto_reconnect_enabled(void);
