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
#include "firmware_contracts.h"

static Preferences prefs;
static WiFiState current_state = WIFI_STATE_DISCONNECTED;
static TaskHandle_t wifi_task_handle = nullptr;
static SemaphoreHandle_t wifi_mutex = nullptr;
static SemaphoreHandle_t prefs_mutex = nullptr;

// Bộ nhớ đệm kết quả quét mạng
static std::vector<WiFiNetworkInfo> scan_results;
static bool scan_in_progress = false;
static bool scan_completed = false;

// Thông tin mạng yêu cầu kết nối
static char target_ssid[33] = {0};
static char target_pass[65] = {0};
static bool connect_requested = false;
static uint32_t connect_start_time = 0;
static bool should_save_credentials = false;
static uint32_t request_generation = 1;
static uint32_t active_connect_generation = 0;
static uint32_t pending_save_generation = 0;
static uint32_t reconnect_backoff_ms = 2000;
static uint32_t last_disconnect_time = 0;
static bool auto_reconnect_enabled = (WIFI_AUTO_RECONNECT != 0);
static bool manual_disconnect = false;
static char last_error[96] = "";

static void set_error_locked(const char *message)
{
    strlcpy(last_error, message ? message : "Unknown WiFi error", sizeof(last_error));
}

static void lock_wifi()
{
    if (wifi_mutex) xSemaphoreTake(wifi_mutex, portMAX_DELAY);
}

static void unlock_wifi()
{
    if (wifi_mutex) xSemaphoreGive(wifi_mutex);
}

static void lock_prefs()
{
    if (prefs_mutex) xSemaphoreTake(prefs_mutex, portMAX_DELAY);
}

static void unlock_prefs()
{
    if (prefs_mutex) xSemaphoreGive(prefs_mutex);
}

static uint32_t next_generation_locked()
{
    ++request_generation;
    if (request_generation == 0) ++request_generation;
    return request_generation;
}

static bool save_credentials_for_generation(uint32_t generation, const char *ssid, const char *pass)
{
    if (!prefs_mutex || !wifi_mutex || !ssid || !*ssid) return false;
    // prefs_mutex serializes Save with Forget. Generation is checked while that
    // serialization lock is held, so an obsolete Save cannot resurrect a network.
    lock_prefs();
    lock_wifi();
    const bool current = should_save_credentials && wifi_generation_can_commit(
        generation, request_generation, pending_save_generation, manual_disconnect);
    unlock_wifi();
    bool ok = false;
    if (current && prefs.begin(WIFI_PREFS_NAMESPACE, false))
    {
        ok = prefs.putString(WIFI_PREFS_KEY_SSID, ssid) == strlen(ssid);
        const char *safe_pass = pass ? pass : "";
        ok = prefs.putString(WIFI_PREFS_KEY_PASS, safe_pass) == strlen(safe_pass) && ok;
        prefs.end();
    }
    unlock_prefs();

    lock_wifi();
    if (generation == request_generation && generation == pending_save_generation)
    {
        should_save_credentials = false;
        pending_save_generation = 0;
    }
    unlock_wifi();
    return ok;
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
        wifi_manager_connect(saved_ssid.c_str(), saved_pass.c_str(), false); // false: Đã có trong NVS, không ghi lại
    }
    else if (strlen(DEFAULT_WIFI_SSID) > 0)
    {
        log_i("Tìm thấy DEFAULT_WIFI_SSID: %s, tiến hành kết nối...", DEFAULT_WIFI_SSID);
        wifi_manager_connect(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, false);
    }

    while (1)
    {
        // Xử lý yêu cầu kết nối mới
        bool begin_connect = false;
        char connect_ssid[33] = {};
        char connect_pass[65] = {};
        uint32_t connect_generation = 0;
        lock_wifi();
        if (connect_requested)
        {
            connect_requested = false;
            current_state = WIFI_STATE_CONNECTING;
            connect_start_time = millis();
            active_connect_generation = request_generation;
            connect_generation = active_connect_generation;
            strlcpy(connect_ssid, target_ssid, sizeof(connect_ssid));
            strlcpy(connect_pass, target_pass, sizeof(connect_pass));
            begin_connect = true;
        }
        unlock_wifi();
        if (begin_connect)
        {
            log_i("Bắt đầu kết nối WiFi generation=%u SSID=%s", connect_generation, connect_ssid);
            WiFi.disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            WiFi.begin(connect_ssid, connect_pass);
        }

        // Kiểm tra tiến độ kết nối
        lock_wifi();
        const WiFiState state_snapshot = current_state;
        const uint32_t active_generation_snapshot = active_connect_generation;
        const uint32_t connect_started_snapshot = connect_start_time;
        unlock_wifi();
        if (state_snapshot == WIFI_STATE_CONNECTING)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                bool need_save = false;
                char save_s[33] = {0};
                char save_p[65] = {0};

                lock_wifi();
                const bool still_current = active_generation_snapshot == request_generation &&
                                           active_generation_snapshot == active_connect_generation &&
                                           !manual_disconnect;
                if (still_current)
                {
                    current_state = WIFI_STATE_CONNECTED;
                    reconnect_backoff_ms = 2000;
                }

                if (still_current && should_save_credentials &&
                    pending_save_generation == active_generation_snapshot)
                {
                    need_save = true;
                    strncpy(save_s, target_ssid, sizeof(save_s) - 1);
                    strncpy(save_p, target_pass, sizeof(save_p) - 1);
                }
                unlock_wifi();

                if (!still_current)
                {
                    WiFi.disconnect();
                    continue;
                }
                log_i("Kết nối WiFi thành công generation=%u IP=%s RSSI=%d dBm",
                      active_generation_snapshot, WiFi.localIP().toString().c_str(), WiFi.RSSI());

                // Lưu NVS Flash ngoài lock_wifi() -> Tuyệt đối không bao giờ deadlock giữa wifi_mutex và prefs_mutex
                if (need_save)
                {
                    if (!save_credentials_for_generation(active_generation_snapshot, save_s, save_p))
                        log_e("WiFi connected but credentials could not be saved");
                }
            }
            else if (millis() - connect_started_snapshot > WIFI_CONNECT_TIMEOUT_MS)
            {
                lock_wifi();
                if (active_generation_snapshot == request_generation &&
                    active_generation_snapshot == active_connect_generation)
                {
                    current_state = WIFI_STATE_FAILED;
                    last_disconnect_time = millis();
                    should_save_credentials = false;
                    pending_save_generation = 0;
                    set_error_locked("Connection timed out");
                    log_w("Kết nối WiFi thất bại: Hết thời gian chờ (Timeout)!");
                }
                unlock_wifi();
            }
        }
        else if (state_snapshot == WIFI_STATE_CONNECTED)
        {
            if (WiFi.status() != WL_CONNECTED)
            {
                lock_wifi();
                current_state = WIFI_STATE_DISCONNECTED;
                last_disconnect_time = millis();
                log_w("Mất kết nối WiFi! Sẽ thử kết nối lại sau %u ms...", reconnect_backoff_ms);
                unlock_wifi();
            }
        }
        else if (state_snapshot == WIFI_STATE_DISCONNECTED || state_snapshot == WIFI_STATE_FAILED)
        {
            // Tự động kết nối lại (Auto-reconnect) với Exponential Backoff (2s -> 4s -> 8s -> ... -> max 60s)
            // Chỉ thực hiện khi người dùng không chủ động Forget/Disconnect và auto reconnect bật
            lock_wifi();
            const bool retry = auto_reconnect_enabled && !manual_disconnect && strlen(target_ssid) > 0 &&
                               (millis() - last_disconnect_time >= reconnect_backoff_ms);
            if (retry)
            {
                log_i("Tự động kết nối lại WiFi '%s' (Backoff: %u ms)...", target_ssid, reconnect_backoff_ms);
                reconnect_backoff_ms = (reconnect_backoff_ms * 2 > 60000) ? 60000 : (reconnect_backoff_ms * 2);
                last_disconnect_time = millis();

                should_save_credentials = false; // Không spam ghi NVS khi reconnect
                pending_save_generation = 0;
                next_generation_locked();
                connect_requested = true;
            }
            unlock_wifi();
        }

        // Xử lý quét mạng bất đồng bộ
        lock_wifi();
        const bool scan_active = scan_in_progress;
        unlock_wifi();
        if (scan_active)
        {
            int n = WiFi.scanComplete();
            if (n >= 0)
            {
                std::vector<WiFiNetworkInfo> completed_results;
                completed_results.reserve(n);
                for (int i = 0; i < n; ++i)
                {
                    WiFiNetworkInfo net;
                    strncpy(net.ssid, WiFi.SSID(i).c_str(), sizeof(net.ssid) - 1);
                    net.ssid[sizeof(net.ssid) - 1] = '\0';
                    net.rssi = WiFi.RSSI(i);
                    net.channel = WiFi.channel(i);
                    net.is_encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
                    completed_results.push_back(net);
                }
                WiFi.scanDelete();
                lock_wifi();
                scan_results.swap(completed_results);
                scan_in_progress = false;
                scan_completed = true;
                last_error[0] = '\0';
                log_i("Quét hoàn tất: Tìm thấy %d mạng WiFi", n);
                unlock_wifi();
            }
            else if (n == WIFI_SCAN_FAILED)
            {
                WiFi.scanDelete();
                lock_wifi();
                scan_in_progress = false;
                scan_completed = true;
                set_error_locked("WiFi scan failed");
                unlock_wifi();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool wifi_manager_init(void)
{
    if (!wifi_mutex) wifi_mutex = xSemaphoreCreateMutex();
    if (!prefs_mutex) prefs_mutex = xSemaphoreCreateMutex();
    if (!wifi_mutex || !prefs_mutex)
    {
        current_state = WIFI_STATE_FAILED;
        strlcpy(last_error, "Cannot create WiFi mutex", sizeof(last_error));
        log_e("WiFi degraded: không tạo được mutex");
        return false;
    }

    // Khởi tạo Task FreeRTOS ghim cố định trên Core 0
    BaseType_t created = xTaskCreatePinnedToCore(
        wifi_service_task,
        "WiFi_Task",
        4 * 1024,
        nullptr,
        2,
        &wifi_task_handle,
        0 // Core 0
    );
    if (created != pdPASS)
    {
        wifi_task_handle = nullptr;
        current_state = WIFI_STATE_FAILED;
        strlcpy(last_error, "Cannot create WiFi service task", sizeof(last_error));
        log_e("WiFi degraded: không tạo được service task");
        return false;
    }
    return true;
}

bool wifi_manager_scan_async(void)
{
    if (!wifi_mutex) return false;
    lock_wifi();
    if (scan_in_progress)
    {
        unlock_wifi();
        return true;
    }
    scan_completed = false;
    scan_results.clear();
    scan_in_progress = true;
    last_error[0] = '\0';
    unlock_wifi();
    const int result = WiFi.scanNetworks(true);
    if (result == WIFI_SCAN_FAILED)
    {
        lock_wifi();
        scan_in_progress = false;
        scan_completed = true;
        set_error_locked("Cannot start WiFi scan");
        unlock_wifi();
        return false;
    }
    log_i("Bắt đầu quét mạng WiFi...");
    return true;
}

bool wifi_manager_is_scan_done(void)
{
    lock_wifi();
    bool done = scan_completed;
    unlock_wifi();
    return done;
}

String wifi_manager_get_last_error(void)
{
    lock_wifi();
    String error(last_error);
    unlock_wifi();
    return error;
}

std::vector<WiFiNetworkInfo> wifi_manager_get_scan_results(void)
{
    lock_wifi();
    std::vector<WiFiNetworkInfo> res = scan_results;
    unlock_wifi();
    return res;
}

bool wifi_manager_connect(const char *ssid, const char *pass, bool save_to_nvs)
{
    if (!wifi_mutex || ssid == nullptr || strlen(ssid) == 0 || strlen(ssid) > 32 ||
        (pass && strlen(pass) > 64)) return false;

    lock_wifi();
    const uint32_t generation = next_generation_locked();
    manual_disconnect = false;
    reconnect_backoff_ms = 2000;
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
    should_save_credentials = save_to_nvs; // Chỉ lưu NVS khi người dùng chủ động cấu hình
    pending_save_generation = save_to_nvs ? generation : 0;
    last_error[0] = '\0';
    unlock_wifi();

    return true;
}


void wifi_manager_disconnect(void)
{
    lock_wifi();
    next_generation_locked();
    manual_disconnect = true;
    connect_requested = false;
    should_save_credentials = false;
    pending_save_generation = 0;
    target_ssid[0] = '\0';
    target_pass[0] = '\0';
    current_state = WIFI_STATE_DISCONNECTED;
    unlock_wifi();
    WiFi.disconnect();
    log_i("Đã ngắt kết nối WiFi thủ công (Đã xóa runtime target & vô hiệu hóa auto-reconnect)");
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
    if (!prefs_mutex) return false;
    lock_prefs();
    String ssid;
    if (prefs.begin(WIFI_PREFS_NAMESPACE, true))
    {
        ssid = prefs.getString(WIFI_PREFS_KEY_SSID, "");
        prefs.end();
    }
    unlock_prefs();
    return (ssid.length() > 0);
}

bool wifi_manager_save_credentials(const char *ssid, const char *pass)
{
    if (!prefs_mutex || !ssid || strlen(ssid) == 0) return false;
    lock_prefs();
    bool ok = prefs.begin(WIFI_PREFS_NAMESPACE, false);
    if (ok)
    {
        ok = prefs.putString(WIFI_PREFS_KEY_SSID, ssid) == strlen(ssid);
        const char *safe_pass = pass ? pass : "";
        ok = prefs.putString(WIFI_PREFS_KEY_PASS, safe_pass) == strlen(safe_pass) && ok;
        prefs.end();
    }
    unlock_prefs();
    if (ok) log_i("Đã lưu thông tin WiFi [%s] vào NVS Flash", ssid);
    return ok;
}

bool wifi_manager_load_credentials(String &ssid, String &pass)
{
    if (!prefs_mutex) return false;
    lock_prefs();
    bool ok = prefs.begin(WIFI_PREFS_NAMESPACE, true);
    if (ok)
    {
        ssid = prefs.getString(WIFI_PREFS_KEY_SSID, "");
        pass = prefs.getString(WIFI_PREFS_KEY_PASS, "");
        prefs.end();
    }
    unlock_prefs();
    return ok && ssid.length() > 0;
}

bool wifi_manager_clear_credentials(void)
{
    if (!prefs_mutex) return false;
    lock_prefs();
    bool ok = prefs.begin(WIFI_PREFS_NAMESPACE, false);
    if (ok)
    {
        bool ssid_ok = !prefs.isKey(WIFI_PREFS_KEY_SSID) || prefs.remove(WIFI_PREFS_KEY_SSID);
        bool pass_ok = !prefs.isKey(WIFI_PREFS_KEY_PASS) || prefs.remove(WIFI_PREFS_KEY_PASS);
        ok = ssid_ok && pass_ok;
        prefs.end();
    }
    unlock_prefs();
    if (ok) log_i("Đã xóa thông tin WiFi trong NVS Flash");
    return ok;
}

bool wifi_manager_forget_network(void)
{
    lock_wifi();
    next_generation_locked();
    manual_disconnect = true;
    connect_requested = false;
    should_save_credentials = false;
    pending_save_generation = 0;
    active_connect_generation = 0;
    target_ssid[0] = '\0';
    target_pass[0] = '\0';
    current_state = WIFI_STATE_DISCONNECTED;
    unlock_wifi();
    WiFi.disconnect();
    // Clear uses the same prefs owner as generation-aware Save. If an old Save
    // was already committing, this clear runs after it and wins deterministically.
    const bool cleared = wifi_manager_clear_credentials();
    if (cleared) log_i("Đã quên mạng WiFi hiện tại: NVS đã xóa, runtime target đã dọn sạch, ngắt kết nối an toàn.");
    return cleared;
}

void wifi_manager_set_auto_reconnect(bool enable)
{
    lock_wifi();
    auto_reconnect_enabled = enable;
    unlock_wifi();
    log_i("Cấu hình WiFi Auto-Reconnect: %s", enable ? "BẬT" : "TẮT");
}

bool wifi_manager_is_auto_reconnect_enabled(void)
{
    lock_wifi();
    const bool enabled = auto_reconnect_enabled;
    unlock_wifi();
    return enabled;
}
