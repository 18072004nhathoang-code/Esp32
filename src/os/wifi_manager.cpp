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
#include "service_state_logic.h"

static Preferences prefs;
static WiFiState current_state = WIFI_STATE_DISCONNECTED;
static TaskHandle_t wifi_task_handle = nullptr;
static SemaphoreHandle_t wifi_mutex = nullptr;
static SemaphoreHandle_t prefs_mutex = nullptr;
static QueueHandle_t wifi_command_queue = nullptr;

enum WiFiCommandType : uint8_t
{
    WIFI_CMD_CONNECT = 1,
    WIFI_CMD_DISCONNECT,
    WIFI_CMD_FORGET,
    WIFI_CMD_SCAN,
    WIFI_CMD_SAVE_ONLY,
    WIFI_CMD_CLEAR_ONLY
};

struct WiFiCommand
{
    WiFiCommandType type;
    uint32_t generation;
    bool save_to_nvs;
    char ssid[33];
    char pass[65];
};

// Bộ nhớ đệm kết quả quét mạng
static std::vector<WiFiNetworkInfo> scan_results;
static bool scan_in_progress = false;
static bool scan_completed = false;

// Thông tin mạng yêu cầu kết nối
static char target_ssid[33] = {0};
static char target_pass[65] = {0};
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
static char connected_ssid[33] = "";
static char connected_ip[16] = "0.0.0.0";
static int8_t connected_rssi = 0;

static bool enqueue_wifi_command(const WiFiCommand &cmd)
{
    return wifi_command_queue && xQueueSend(wifi_command_queue, &cmd, 0) == pdTRUE;
}

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

static bool save_credentials_direct(uint32_t generation, const char *ssid, const char *pass)
{
    if (!prefs_mutex || !wifi_mutex || generation == 0 || !ssid || !*ssid) return false;
    lock_prefs();
    lock_wifi();
    const bool current = generation == request_generation;
    unlock_wifi();
    bool ok = current && prefs.begin(WIFI_PREFS_NAMESPACE, false);
    if (ok)
    {
        ok = prefs.putString(WIFI_PREFS_KEY_SSID, ssid) == strlen(ssid);
        const char *safe_pass = pass ? pass : "";
        ok = prefs.putString(WIFI_PREFS_KEY_PASS, safe_pass) == strlen(safe_pass) && ok;
        prefs.end();
    }
    unlock_prefs();
    return ok;
}

static bool clear_credentials_direct(void)
{
    if (!prefs_mutex) return false;
    lock_prefs();
    bool ok = prefs.begin(WIFI_PREFS_NAMESPACE, false);
    if (ok)
    {
        const bool ssid_ok = !prefs.isKey(WIFI_PREFS_KEY_SSID) || prefs.remove(WIFI_PREFS_KEY_SSID);
        const bool pass_ok = !prefs.isKey(WIFI_PREFS_KEY_PASS) || prefs.remove(WIFI_PREFS_KEY_PASS);
        ok = ssid_ok && pass_ok;
        prefs.end();
    }
    unlock_prefs();
    return ok;
}

static void clear_connected_cache_locked(void)
{
    connected_ssid[0] = '\0';
    strlcpy(connected_ip, "0.0.0.0", sizeof(connected_ip));
    connected_rssi = 0;
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
        WiFiCommand command = {};
        while (wifi_command_queue && xQueueReceive(wifi_command_queue, &command, 0) == pdTRUE)
        {
            if (command.type == WIFI_CMD_CONNECT)
            {
                lock_wifi();
                bool current = service_generation_current(
                    command.generation, request_generation, manual_disconnect);
                const bool cancel_scan = scan_in_progress;
                if (cancel_scan)
                {
                    scan_in_progress = false;
                    scan_completed = true;
                }
                unlock_wifi();
                if (!current) continue;

                if (cancel_scan) WiFi.scanDelete();
                WiFi.disconnect();
                vTaskDelay(pdMS_TO_TICKS(100));

                // The API invalidates generation immediately. Revalidate after
                // the delay and immediately before the irreversible begin().
                lock_wifi();
                current = service_generation_current(
                    command.generation, request_generation, manual_disconnect);
                if (current)
                {
                    active_connect_generation = command.generation;
                    connect_start_time = millis();
                    current_state = WIFI_STATE_CONNECTING;
                }
                unlock_wifi();
                if (!current)
                {
                    WiFi.disconnect();
                    continue;
                }
                log_i("Bắt đầu kết nối WiFi generation=%u SSID=%s",
                      command.generation, command.ssid);
                WiFi.begin(command.ssid, command.pass);
            }
            else if (command.type == WIFI_CMD_DISCONNECT || command.type == WIFI_CMD_FORGET)
            {
                lock_wifi();
                const bool current = command.generation == request_generation;
                unlock_wifi();
                if (!current) continue;
                WiFi.disconnect();
                const bool cleared = command.type != WIFI_CMD_FORGET || clear_credentials_direct();
                lock_wifi();
                if (command.generation == request_generation)
                {
                    current_state = cleared ? WIFI_STATE_DISCONNECTED : WIFI_STATE_FAILED;
                    active_connect_generation = 0;
                    clear_connected_cache_locked();
                    if (!cleared) set_error_locked("Cannot clear WiFi credentials");
                }
                unlock_wifi();
                if (command.type == WIFI_CMD_FORGET)
                    log_i("WiFi forget command applied");
                else
                    log_i("WiFi disconnect command applied");
            }
            else if (command.type == WIFI_CMD_SCAN)
            {
                const int result = WiFi.scanNetworks(true);
                if (result == WIFI_SCAN_FAILED)
                {
                    lock_wifi();
                    scan_in_progress = false;
                    scan_completed = true;
                    set_error_locked("Cannot start WiFi scan");
                    unlock_wifi();
                }
            }
            else if (command.type == WIFI_CMD_SAVE_ONLY)
            {
                if (!save_credentials_direct(command.generation, command.ssid, command.pass))
                {
                    lock_wifi();
                    set_error_locked("Cannot save WiFi credentials");
                    unlock_wifi();
                }
            }
            else if (command.type == WIFI_CMD_CLEAR_ONLY)
            {
                if (!clear_credentials_direct())
                {
                    lock_wifi();
                    set_error_locked("Cannot clear WiFi credentials");
                    unlock_wifi();
                }
            }
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
                const bool still_current = service_generation_current(
                    active_generation_snapshot, request_generation, manual_disconnect) &&
                    active_generation_snapshot == active_connect_generation;
                if (still_current)
                {
                    current_state = WIFI_STATE_CONNECTED;
                    reconnect_backoff_ms = 2000;
                    strlcpy(connected_ssid, WiFi.SSID().c_str(), sizeof(connected_ssid));
                    strlcpy(connected_ip, WiFi.localIP().toString().c_str(), sizeof(connected_ip));
                    connected_rssi = WiFi.RSSI();
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
                bool disconnect_timed_out = false;
                lock_wifi();
                if (active_generation_snapshot == request_generation &&
                    active_generation_snapshot == active_connect_generation)
                {
                    current_state = WIFI_STATE_FAILED;
                    clear_connected_cache_locked();
                    last_disconnect_time = millis();
                    should_save_credentials = false;
                    pending_save_generation = 0;
                    set_error_locked("Connection timed out");
                    log_w("Kết nối WiFi thất bại: Hết thời gian chờ (Timeout)!");
                    disconnect_timed_out = true;
                }
                unlock_wifi();
                if (disconnect_timed_out) WiFi.disconnect();
            }
        }
        else if (state_snapshot == WIFI_STATE_CONNECTED)
        {
            if (WiFi.status() != WL_CONNECTED)
            {
                lock_wifi();
                current_state = WIFI_STATE_DISCONNECTED;
                clear_connected_cache_locked();
                last_disconnect_time = millis();
                log_w("Mất kết nối WiFi! Sẽ thử kết nối lại sau %u ms...", reconnect_backoff_ms);
                unlock_wifi();
            }
            else
            {
                lock_wifi();
                connected_rssi = WiFi.RSSI();
                strlcpy(connected_ip, WiFi.localIP().toString().c_str(), sizeof(connected_ip));
                unlock_wifi();
            }
        }
        else if (state_snapshot == WIFI_STATE_DISCONNECTED || state_snapshot == WIFI_STATE_FAILED)
        {
            // Tự động kết nối lại (Auto-reconnect) với Exponential Backoff (2s -> 4s -> 8s -> ... -> max 60s)
            // Chỉ thực hiện khi người dùng không chủ động Forget/Disconnect và auto reconnect bật
            WiFiCommand retry_cmd = {};
            bool enqueue_retry = false;
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
                retry_cmd.type = WIFI_CMD_CONNECT;
                retry_cmd.generation = next_generation_locked();
                strlcpy(retry_cmd.ssid, target_ssid, sizeof(retry_cmd.ssid));
                strlcpy(retry_cmd.pass, target_pass, sizeof(retry_cmd.pass));
                enqueue_retry = true;
            }
            unlock_wifi();
            if (enqueue_retry && !enqueue_wifi_command(retry_cmd))
            {
                lock_wifi();
                current_state = WIFI_STATE_FAILED;
                set_error_locked("WiFi reconnect queue full");
                unlock_wifi();
            }
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
    if (wifi_task_handle) return true;
    if (!wifi_mutex) wifi_mutex = xSemaphoreCreateMutex();
    if (!prefs_mutex) prefs_mutex = xSemaphoreCreateMutex();
    if (!wifi_command_queue) wifi_command_queue = xQueueCreate(8, sizeof(WiFiCommand));
    if (!wifi_mutex || !prefs_mutex || !wifi_command_queue)
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
    if (!wifi_mutex || !wifi_command_queue) return false;
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
    WiFiCommand cmd = {};
    cmd.type = WIFI_CMD_SCAN;
    if (!enqueue_wifi_command(cmd))
    {
        lock_wifi();
        scan_in_progress = false;
        scan_completed = true;
        set_error_locked("WiFi command queue full");
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

    WiFiCommand cmd = {};
    cmd.type = WIFI_CMD_CONNECT;
    cmd.save_to_nvs = save_to_nvs;
    strlcpy(cmd.ssid, ssid, sizeof(cmd.ssid));
    strlcpy(cmd.pass, pass ? pass : "", sizeof(cmd.pass));

    lock_wifi();
    const uint32_t generation = next_generation_locked();
    cmd.generation = generation;
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

    current_state = WIFI_STATE_CONNECTING;
    should_save_credentials = save_to_nvs; // Chỉ lưu NVS khi người dùng chủ động cấu hình
    pending_save_generation = save_to_nvs ? generation : 0;
    last_error[0] = '\0';
    unlock_wifi();
    if (enqueue_wifi_command(cmd)) return true;
    lock_wifi();
    if (request_generation == generation)
    {
        current_state = WIFI_STATE_FAILED;
        should_save_credentials = false;
        pending_save_generation = 0;
        set_error_locked("WiFi command queue full");
    }
    unlock_wifi();
    return false;
}


void wifi_manager_disconnect(void)
{
    WiFiCommand cmd = {};
    cmd.type = WIFI_CMD_DISCONNECT;
    lock_wifi();
    cmd.generation = next_generation_locked();
    manual_disconnect = true;
    should_save_credentials = false;
    pending_save_generation = 0;
    target_ssid[0] = '\0';
    target_pass[0] = '\0';
    current_state = WIFI_STATE_DISCONNECTING;
    unlock_wifi();
    if (!enqueue_wifi_command(cmd))
    {
        lock_wifi();
        current_state = WIFI_STATE_FAILED;
        set_error_locked("WiFi disconnect queue full");
        unlock_wifi();
    }
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
    lock_wifi();
    const bool connected = current_state == WIFI_STATE_CONNECTED;
    unlock_wifi();
    return connected;
}

String wifi_manager_get_ip(void)
{
    lock_wifi();
    String value(connected_ip);
    unlock_wifi();
    return value;
}

String wifi_manager_get_ssid(void)
{
    lock_wifi();
    String value(connected_ssid);
    unlock_wifi();
    return value;
}

int8_t wifi_manager_get_rssi(void)
{
    lock_wifi();
    const int8_t value = connected_rssi;
    unlock_wifi();
    return value;
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
    if (!wifi_command_queue || !ssid || strlen(ssid) == 0 || strlen(ssid) > 32 ||
        (pass && strlen(pass) > 64)) return false;
    WiFiCommand cmd = {};
    cmd.type = WIFI_CMD_SAVE_ONLY;
    strlcpy(cmd.ssid, ssid, sizeof(cmd.ssid));
    strlcpy(cmd.pass, pass ? pass : "", sizeof(cmd.pass));
    lock_wifi();
    cmd.generation = request_generation;
    unlock_wifi();
    return enqueue_wifi_command(cmd);
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
    if (!wifi_command_queue) return false;
    WiFiCommand cmd = {};
    cmd.type = WIFI_CMD_CLEAR_ONLY;
    return enqueue_wifi_command(cmd);
}

bool wifi_manager_forget_network(void)
{
    if (!wifi_command_queue) return false;
    WiFiCommand cmd = {};
    cmd.type = WIFI_CMD_FORGET;
    lock_wifi();
    cmd.generation = next_generation_locked();
    manual_disconnect = true;
    should_save_credentials = false;
    pending_save_generation = 0;
    active_connect_generation = 0;
    target_ssid[0] = '\0';
    target_pass[0] = '\0';
    current_state = WIFI_STATE_FORGETTING;
    unlock_wifi();
    if (enqueue_wifi_command(cmd)) return true;
    lock_wifi();
    current_state = WIFI_STATE_FAILED;
    set_error_locked("WiFi forget queue full");
    unlock_wifi();
    return false;
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
