/**
 * @file wifi_manager.cpp
 * @brief Triển khai dịch vụ WiFi chạy nền trên Core 0 với NVS Storage cho ESP32-S3
 */

#include "wifi_manager.h"
#include "wifi_config.h"
#include <WiFi.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static Preferences prefs;
static WiFiState current_state = WIFI_STATE_DISCONNECTED;
static TaskHandle_t wifi_task_handle = nullptr;
static SemaphoreHandle_t wifi_mutex = nullptr;

// Bộ nhớ đệm kết quả quét mạng
static std::vector<WiFiNetworkInfo> scan_results;
static bool scan_in_progress = false;
static bool scan_completed = false;

// Thông tin mạng yêu cầu kết nối
static char target_ssid[33] = {0};
static char target_pass[65] = {0};
static bool connect_requested = false;
static uint32_t connect_start_time = 0;

static void lock_wifi()
{
    if (wifi_mutex) xSemaphoreTake(wifi_mutex, portMAX_DELAY);
}

static void unlock_wifi()
{
    if (wifi_mutex) xSemaphoreGive(wifi_mutex);
}

/* Task chuyên trách quản lý mạng WiFi chạy độc lập trên Core 0 */
static void wifi_service_task(void *pvParameters)
{
    log_i("WiFi Service Task đang chạy trên Core %d", xPortGetCoreID());

    // Thiết lập chế độ Station
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    // 1. Kiểm tra xem có cấu hình WiFi lưu trong NVS hoặc cấu hình mặc định không
    String saved_ssid, saved_pass;
    if (wifi_manager_load_credentials(saved_ssid, saved_pass) && saved_ssid.length() > 0)
    {
        log_i("Tìm thấy thông tin WiFi trong NVS: %s, tiến hành tự động kết nối...", saved_ssid.c_str());
        wifi_manager_connect(saved_ssid.c_str(), saved_pass.c_str());
    }
    else if (strlen(DEFAULT_WIFI_SSID) > 0)
    {
        log_i("Tìm thấy DEFAULT_WIFI_SSID: %s, tiến hành kết nối...", DEFAULT_WIFI_SSID);
        wifi_manager_connect(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
    }

    while (1)
    {
        // Xử lý yêu cầu kết nối mới
        if (connect_requested)
        {
            lock_wifi();
            connect_requested = false;
            current_state = WIFI_STATE_CONNECTING;
            connect_start_time = millis();
            log_i("Bắt đầu kết nối tới WiFi: %s", target_ssid);
            WiFi.disconnect();
            delay(100);
            WiFi.begin(target_ssid, target_pass);
            unlock_wifi();
        }

        // Kiểm tra tiến độ kết nối
        if (current_state == WIFI_STATE_CONNECTING)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                lock_wifi();
                current_state = WIFI_STATE_CONNECTED;
                log_i("Kết nối WiFi thành công! IP: %s, RSSI: %d dBm", 
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
                // Lưu vĩnh viễn vào NVS Flash
                wifi_manager_save_credentials(target_ssid, target_pass);
                unlock_wifi();
            }
            else if (millis() - connect_start_time > WIFI_CONNECT_TIMEOUT_MS)
            {
                lock_wifi();
                current_state = WIFI_STATE_FAILED;
                log_w("Kết nối WiFi thất bại: Hết thời gian chờ (Timeout)!");
                unlock_wifi();
            }
        }
        else if (current_state == WIFI_STATE_CONNECTED)
        {
            if (WiFi.status() != WL_CONNECTED)
            {
                lock_wifi();
                current_state = WIFI_STATE_DISCONNECTED;
                log_w("Mất kết nối WiFi!");
                unlock_wifi();
            }
        }

        // Xử lý quét mạng bất đồng bộ
        if (scan_in_progress)
        {
            int n = WiFi.scanComplete();
            if (n >= 0)
            {
                lock_wifi();
                scan_results.clear();
                for (int i = 0; i < n; ++i)
                {
                    WiFiNetworkInfo net;
                    strncpy(net.ssid, WiFi.SSID(i).c_str(), sizeof(net.ssid) - 1);
                    net.ssid[sizeof(net.ssid) - 1] = '\0';
                    net.rssi = WiFi.RSSI(i);
                    net.channel = WiFi.channel(i);
                    net.is_encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
                    scan_results.push_back(net);
                }
                WiFi.scanDelete();
                scan_in_progress = false;
                scan_completed = true;
                log_i("Quét hoàn tất: Tìm thấy %d mạng WiFi", n);
                unlock_wifi();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void wifi_manager_init(void)
{
    wifi_mutex = xSemaphoreCreateMutex();

    // Khởi tạo Task FreeRTOS ghim cố định trên Core 0
    xTaskCreatePinnedToCore(
        wifi_service_task,
        "WiFi_Task",
        4 * 1024,
        nullptr,
        2,
        &wifi_task_handle,
        0 // Core 0
    );
}

void wifi_manager_scan_async(void)
{
    lock_wifi();
    if (!scan_in_progress)
    {
        scan_completed = false;
        scan_in_progress = true;
        WiFi.scanNetworks(true); // true = async scan
        log_i("Bắt đầu quét mạng WiFi...");
    }
    unlock_wifi();
}

bool wifi_manager_is_scan_done(void)
{
    lock_wifi();
    bool done = scan_completed;
    unlock_wifi();
    return done;
}

std::vector<WiFiNetworkInfo> wifi_manager_get_scan_results(void)
{
    lock_wifi();
    std::vector<WiFiNetworkInfo> res = scan_results;
    unlock_wifi();
    return res;
}

bool wifi_manager_connect(const char *ssid, const char *pass)
{
    if (ssid == nullptr || strlen(ssid) == 0) return false;

    lock_wifi();
    strncpy(target_ssid, ssid, sizeof(target_ssid) - 1);
    target_ssid[sizeof(target_ssid) - 1] = '\0';

    if (pass != nullptr)
    {
        strncpy(target_pass, pass, sizeof(target_pass) - 1);
        target_pass[sizeof(target_pass) - 1] = '\0';
    }
    else
    {
        target_pass[0] = '\0';
    }

    connect_requested = true;
    current_state = WIFI_STATE_CONNECTING;
    unlock_wifi();

    return true;
}

void wifi_manager_disconnect(void)
{
    lock_wifi();
    WiFi.disconnect();
    current_state = WIFI_STATE_DISCONNECTED;
    unlock_wifi();
}

WiFiState wifi_manager_get_state(void)
{
    lock_wifi();
    WiFiState state = current_state;
    unlock_wifi();
    return state;
}

bool wifi_manager_is_connected(void)
{
    return (WiFi.status() == WL_CONNECTED);
}

String wifi_manager_get_ip(void)
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return WiFi.localIP().toString();
    }
    return "0.0.0.0";
}

String wifi_manager_get_ssid(void)
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return WiFi.SSID();
    }
    return "";
}

int8_t wifi_manager_get_rssi(void)
{
    if (WiFi.status() == WL_CONNECTED)
    {
        return WiFi.RSSI();
    }
    return 0;
}

bool wifi_manager_has_saved_credentials(void)
{
    prefs.begin(WIFI_PREFS_NAMESPACE, true);
    String ssid = prefs.getString(WIFI_PREFS_KEY_SSID, "");
    prefs.end();
    return (ssid.length() > 0);
}

void wifi_manager_save_credentials(const char *ssid, const char *pass)
{
    if (!ssid || strlen(ssid) == 0) return;
    prefs.begin(WIFI_PREFS_NAMESPACE, false);
    prefs.putString(WIFI_PREFS_KEY_SSID, ssid);
    prefs.putString(WIFI_PREFS_KEY_PASS, pass ? pass : "");
    prefs.end();
    log_i("Đã lưu thông tin WiFi [%s] vào NVS Flash", ssid);
}

bool wifi_manager_load_credentials(String &ssid, String &pass)
{
    prefs.begin(WIFI_PREFS_NAMESPACE, true);
    ssid = prefs.getString(WIFI_PREFS_KEY_SSID, "");
    pass = prefs.getString(WIFI_PREFS_KEY_PASS, "");
    prefs.end();
    return (ssid.length() > 0);
}

void wifi_manager_clear_credentials(void)
{
    prefs.begin(WIFI_PREFS_NAMESPACE, false);
    prefs.remove(WIFI_PREFS_KEY_SSID);
    prefs.remove(WIFI_PREFS_KEY_PASS);
    prefs.end();
    log_i("Đã xóa thông tin WiFi trong NVS Flash");
}
