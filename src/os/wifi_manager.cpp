/**
 * @file wifi_manager.cpp
 * @brief Triển khai dịch vụ WiFi chạy nền trên Core 0 với NVS Storage cho ESP32-S3
 */

#include "wifi_manager.h"
#include "wifi_config.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "firmware_contracts.h"
#include "service_state_logic.h"
#include "wifi_scan_adapter.h"
#include "wifi_storage_logic.h"

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

static bool pending_disconnect_control = false;
static bool pending_forget_control = false;
static WiFiCommand pending_disconnect_command = {};
static WiFiCommand pending_forget_command = {};
static WiFiControlStatus control_status = WIFI_CONTROL_NONE;
static uint32_t control_ack_generation = 0;

// Scan state is independent from connection state. Only wifi_service_task calls
// the Arduino/IDF radio scan APIs.
static WifiScanCoordinator scan_control;
static WiFiNetworkInfo scan_results[WIFI_SCAN_MAX_RESULTS] = {};
static size_t scan_result_count = 0;
static char scan_error[96] = "";
static int32_t scan_driver_error = ESP_OK;
static WifiScanDriverAdapter scan_driver;
static uint32_t scan_driver_request_id = 0;
static uint8_t scan_start_attempts = 0;
static uint32_t scan_retry_at_ms = 0;
static WiFiNetworkInfo scan_copy_buffer[WIFI_SCAN_MAX_RESULTS] = {};
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 10000;
static constexpr uint32_t WIFI_SCAN_TOTAL_BUDGET_MS = 20000;
static constexpr uint32_t WIFI_SCAN_CONNECT_WAIT_MS = 4000;
static constexpr uint32_t WIFI_SCAN_DRAIN_TIMEOUT_MS = 2000;
static constexpr uint32_t WIFI_SCAN_RETRY_BACKOFF_MS = 300;
static constexpr uint8_t WIFI_SCAN_MAX_START_ATTEMPTS = 3;

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
static char connection_error[96] = "";
static char connected_ssid[33] = "";
static char connected_ip[16] = "0.0.0.0";
static int8_t connected_rssi = 0;

static bool enqueue_wifi_command(const WiFiCommand &cmd)
{
    return wifi_command_queue && xQueueSend(wifi_command_queue, &cmd, 0) == pdTRUE;
}

static void set_error_locked(const char *message)
{
    strlcpy(connection_error, message ? message : "Unknown WiFi error", sizeof(connection_error));
}

static void set_scan_error_locked(const char *message)
{
    strlcpy(scan_error, message ? message : "Unknown WiFi scan error", sizeof(scan_error));
}

static void set_scan_driver_error_locked(const char *context, esp_err_t error)
{
    scan_driver_error = static_cast<int32_t>(error);
    snprintf(scan_error, sizeof(scan_error), "%s: %d (%s)",
             context ? context : "WiFi scan driver error",
             static_cast<int>(error), esp_err_to_name(error));
}

static const char *scan_phase_text(WifiScanPhase phase)
{
    switch (phase)
    {
        case WifiScanPhase::IDLE: return "IDLE";
        case WifiScanPhase::QUEUED: return "QUEUED";
        case WifiScanPhase::WAITING_FOR_RADIO: return "WAITING_FOR_RADIO";
        case WifiScanPhase::RUNNING: return "RUNNING";
        case WifiScanPhase::DONE: return "DONE";
        case WifiScanPhase::FAILED: return "FAILED";
        case WifiScanPhase::CANCELED: return "CANCELED";
        default: return "UNKNOWN";
    }
}

static void log_scan_transition_locked(const char *event)
{
    log_i("WiFi scan request=%u event=%s state=%s queued=%u start=%u conn=%d heap=%u largest=%u",
          scan_control.request_id, event, scan_phase_text(scan_control.phase),
          scan_control.queued_at_ms, scan_control.started_at_ms, static_cast<int>(current_state),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

static void log_scan_driver_event(uint32_t request_id, const char *event, esp_err_t error)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    const esp_err_t mode_error = esp_wifi_get_mode(&mode);
    const UBaseType_t stack_words = uxTaskGetStackHighWaterMark(nullptr);
    log_i("WiFi scan request=%u driver=%s err=%d/%s mode=%d mode_err=%d status=%d heap=%u largest=%u stack_free=%uB stale=%u",
          request_id, event, static_cast<int>(error), esp_err_to_name(error),
          static_cast<int>(mode), static_cast<int>(mode_error), static_cast<int>(WiFi.status()),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(stack_words * sizeof(StackType_t)),
          static_cast<unsigned>(scan_driver.stale_event_count()));
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

static bool commit_credentials_to_nvs(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) return false;
    const char *safe_pass = pass ? pass : "";
    const uint32_t new_chk = wifi_credentials_checksum(ssid, safe_pass);

    if (!prefs.begin(WIFI_PREFS_NAMESPACE, false)) return false;

    // 1. Nếu cấu hình hiện tại trong NVS đang hợp lệ, sao lưu sang backup slot
    String cur_ssid = prefs.getString(WIFI_PREFS_KEY_SSID, "");
    String cur_pass = prefs.getString(WIFI_PREFS_KEY_PASS, "");
    uint32_t cur_chk = prefs.getUInt(WIFI_PREFS_KEY_CHK, 0);
    if (cur_ssid.length() > 0 && cur_chk != 0 &&
        cur_chk == wifi_credentials_checksum(cur_ssid.c_str(), cur_pass.c_str()))
    {
        prefs.putString(WIFI_PREFS_KEY_BAK_SSID, cur_ssid);
        prefs.putString(WIFI_PREFS_KEY_BAK_PASS, cur_pass);
        prefs.putUInt(WIFI_PREFS_KEY_BAK_CHK, cur_chk);
    }

    // 2. Vô hiệu hóa checksum của primary slot để chống rách dữ liệu nếu mất nguồn
    prefs.putUInt(WIFI_PREFS_KEY_CHK, 0);

    // 3. Ghi mật khẩu mới
    const size_t pass_len = strlen(safe_pass);
    const bool pass_ok = (prefs.putString(WIFI_PREFS_KEY_PASS, safe_pass) == pass_len);

    // 4. Ghi SSID mới
    const size_t ssid_len = strlen(ssid);
    const bool ssid_ok = (prefs.putString(WIFI_PREFS_KEY_SSID, ssid) == ssid_len);

    // 5. Ghi Checksum mới
    const bool chk_ok = (prefs.putUInt(WIFI_PREFS_KEY_CHK, new_chk) > 0);

    prefs.end();

    if (!pass_ok || !ssid_ok || !chk_ok) return false;

    // 6. Kiểm tra lại dữ liệu vừa ghi
    if (prefs.begin(WIFI_PREFS_NAMESPACE, true))
    {
        const uint32_t read_chk = prefs.getUInt(WIFI_PREFS_KEY_CHK, 0);
        String read_ssid = prefs.getString(WIFI_PREFS_KEY_SSID, "");
        prefs.end();
        return (read_chk == new_chk && read_ssid == ssid);
    }
    return false;
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
    if (current)
    {
        ok = commit_credentials_to_nvs(ssid, pass);
    }
    unlock_prefs();

    lock_wifi();
    if (ok && generation == request_generation && generation == pending_save_generation)
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
    const bool current = (generation == request_generation);
    unlock_wifi();
    bool ok = false;
    if (current)
    {
        ok = commit_credentials_to_nvs(ssid, pass);
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
        if (prefs.isKey(WIFI_PREFS_KEY_SSID)) prefs.remove(WIFI_PREFS_KEY_SSID);
        if (prefs.isKey(WIFI_PREFS_KEY_PASS)) prefs.remove(WIFI_PREFS_KEY_PASS);
        if (prefs.isKey(WIFI_PREFS_KEY_CHK)) prefs.remove(WIFI_PREFS_KEY_CHK);
        if (prefs.isKey(WIFI_PREFS_KEY_BAK_SSID)) prefs.remove(WIFI_PREFS_KEY_BAK_SSID);
        if (prefs.isKey(WIFI_PREFS_KEY_BAK_PASS)) prefs.remove(WIFI_PREFS_KEY_BAK_PASS);
        if (prefs.isKey(WIFI_PREFS_KEY_BAK_CHK)) prefs.remove(WIFI_PREFS_KEY_BAK_CHK);
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

static bool worker_start_connect(const WiFiCommand &command)
{
    lock_wifi();
    bool current = service_generation_current(
        command.generation, request_generation, manual_disconnect);
    unlock_wifi();
    if (!current) return false;

    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    lock_wifi();
    current = service_generation_current(
        command.generation, request_generation, manual_disconnect);
    unlock_wifi();
    if (!current)
    {
        WiFi.disconnect();
        return false;
    }
    log_i("WiFi connect generation=%u SSID=%s", command.generation, command.ssid);
    const wl_status_t begin_status = WiFi.begin(command.ssid, command.pass);
    if (begin_status == WL_CONNECT_FAILED)
    {
        lock_wifi();
        if (service_generation_current(command.generation, request_generation, manual_disconnect))
        {
            current_state = WIFI_STATE_FAILED;
            active_connect_generation = 0;
            last_disconnect_time = millis();
            set_error_locked("WiFi driver rejected connect");
        }
        unlock_wifi();
        return false;
    }

    lock_wifi();
    current = service_generation_current(
        command.generation, request_generation, manual_disconnect);
    if (current)
    {
        active_connect_generation = command.generation;
        connect_start_time = millis();
        current_state = WIFI_STATE_CONNECTING;
        connection_error[0] = '\0';
    }
    unlock_wifi();
    if (!current)
    {
        WiFi.disconnect();
        return false;
    }
    return true;
}

/* Task chuyên trách quản lý mạng WiFi chạy độc lập trên Core 0 */
static void wifi_service_task(void *pvParameters)
{
    log_i("WiFi Service Task đang chạy trên Core %d", xPortGetCoreID());

    // Thiết lập chế độ Station
    WiFi.mode(WIFI_STA);
    // Reconnect is owned exclusively by this worker. Leaving the Arduino core
    // auto-reconnect enabled would create a second radio owner.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true);
    delay(100);
    if (!WiFi.mode(WIFI_STA) || !scan_driver.begin())
    {
        lock_wifi();
        current_state = WIFI_STATE_FAILED;
        set_error_locked("Cannot initialize WiFi scan adapter");
        unlock_wifi();
        log_e("WiFi scan adapter initialization failed");
    }

    // 1. Kiểm tra xem có cấu hình WiFi lưu trong NVS hoặc cấu hình mặc định không
    String saved_ssid, saved_pass;
    if (wifi_manager_load_credentials(saved_ssid, saved_pass) && saved_ssid.length() > 0)
    {
        log_i("Tìm thấy thông tin WiFi trong NVS: %s, tiến hành tự động kết nối...", saved_ssid.c_str());
        wifi_manager_connect(saved_ssid.c_str(), saved_pass.c_str(), false); // false: Đã có trong NVS, không ghi lại
    }
    else if (strlen(DEFAULT_WIFI_SSID) > 0 && !wifi_is_sample_ssid(DEFAULT_WIFI_SSID))
    {
        log_i("Tìm thấy DEFAULT_WIFI_SSID: %s, tiến hành kết nối...", DEFAULT_WIFI_SSID);
        wifi_manager_connect(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS, false);
    }

    WiFiCommand deferred_connect = {};
    bool have_deferred_connect = false;
    while (1)
    {
        // Control mailbox is independent of the ordinary queue. Accepted
        // Disconnect/Forget operations therefore survive a saturated scan/
        // connect queue and are applied by the sole radio/NVS owner.
        WiFiCommand control = {};
        bool have_control = false;
        lock_wifi();
        if (pending_forget_control)
        {
            control = pending_forget_command;
            pending_forget_control = false;
            have_control = true;
        }
        else if (pending_disconnect_control)
        {
            control = pending_disconnect_command;
            pending_disconnect_control = false;
            have_control = true;
        }
        unlock_wifi();
        if (have_control)
        {
            lock_wifi();
            const uint32_t cancel_scan_id = scan_control.busy() ? scan_control.request_id : 0;
            if (cancel_scan_id)
            {
                scan_control.finish(cancel_scan_id, WifiScanPhase::CANCELED);
                set_scan_error_locked("Scan canceled by WiFi control request");
                log_scan_transition_locked("control-cancel");
            }
            unlock_wifi();
            if (scan_driver_request_id)
            {
                const esp_err_t stop_err = scan_driver.stop(scan_driver_request_id, millis());
                log_scan_driver_event(scan_driver_request_id, "control-stop", stop_err);
            }
            const bool disconnect_called = WiFi.disconnect();
            const bool radio_ok = disconnect_called || WiFi.status() != WL_CONNECTED;
            const bool cleared = control.type != WIFI_CMD_FORGET || clear_credentials_direct();
            lock_wifi();
            control_ack_generation = control.generation;
            control_status = (radio_ok && cleared) ? WIFI_CONTROL_APPLIED : WIFI_CONTROL_FAILED;
            if (control.generation == request_generation)
            {
                current_state = (radio_ok && cleared) ? WIFI_STATE_DISCONNECTED : WIFI_STATE_FAILED;
                active_connect_generation = 0;
                clear_connected_cache_locked();
                if (!radio_ok) set_error_locked("WiFi radio disconnect failed");
                else if (!cleared) set_error_locked("Cannot clear WiFi credentials");
            }
            unlock_wifi();
            log_i("WiFi %s generation=%u %s",
                  control.type == WIFI_CMD_FORGET ? "forget" : "disconnect",
                  control.generation, (radio_ok && cleared) ? "APPLIED" : "FAILED");
        }

        WiFiCommand command = {};
        while (wifi_command_queue && xQueueReceive(wifi_command_queue, &command, 0) == pdTRUE)
        {
            if (command.type == WIFI_CMD_CONNECT)
            {
                lock_wifi();
                const bool current = service_generation_current(
                    command.generation, request_generation, manual_disconnect);
                const bool scan_busy = scan_control.busy() ||
                    scan_driver.phase() != WifiScanDriverPhase::IDLE;
                unlock_wifi();
                if (!current) continue;
                if (scan_busy)
                {
                    deferred_connect = command;
                    have_deferred_connect = true;
                    log_i("WiFi connect generation=%u deferred for scan request=%u",
                          command.generation, scan_control.request_id);
                    continue;
                }
                worker_start_connect(command);
            }
            else if (command.type == WIFI_CMD_SCAN)
            {
                lock_wifi();
                if (scan_control.worker_received(command.generation))
                    log_scan_transition_locked("worker-received");
                unlock_wifi();
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

        lock_wifi();
        const bool can_run_deferred = have_deferred_connect && !scan_control.busy() &&
                                      scan_driver.phase() == WifiScanDriverPhase::IDLE;
        unlock_wifi();
        if (can_run_deferred)
        {
            const WiFiCommand command_to_run = deferred_connect;
            have_deferred_connect = false;
            worker_start_connect(command_to_run);
        }

        // Kiểm tra tiến độ kết nối
        lock_wifi();
        const WiFiState state_snapshot = current_state;
        const uint32_t active_generation_snapshot = active_connect_generation;
        const uint32_t connect_started_snapshot = connect_start_time;
        unlock_wifi();
        if (state_snapshot == WIFI_STATE_CONNECTING)
        {
            const bool is_connected = (WiFi.status() == WL_CONNECTED);
            const IPAddress local_ip = WiFi.localIP();
            const bool has_valid_ip = (local_ip != IPAddress(0, 0, 0, 0));

            if (is_connected && has_valid_ip)
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
                    connection_error[0] = '\0';
                    strlcpy(connected_ssid, WiFi.SSID().c_str(), sizeof(connected_ssid));
                    strlcpy(connected_ip, local_ip.toString().c_str(), sizeof(connected_ip));
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
                      active_generation_snapshot, local_ip.toString().c_str(), WiFi.RSSI());

                // Lưu NVS Flash ngoài lock_wifi() -> Tuyệt đối không bao giờ deadlock giữa wifi_mutex và prefs_mutex
                if (need_save)
                {
                    if (save_credentials_for_generation(active_generation_snapshot, save_s, save_p))
                    {
                        log_i("Đã lưu WiFi [%s] vào NVS Flash thành công", save_s);
                    }
                    else
                    {
                        log_e("WiFi connected but credentials could not be saved");
                    }
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
                    const bool was_attempting_new_save = should_save_credentials;
                    should_save_credentials = false;
                    pending_save_generation = 0;
                    set_error_locked("Kết nối thất bại (sai mật khẩu hoặc timeout)");
                    log_w("Kết nối WiFi thất bại: Hết thời gian chờ (Timeout)!");
                    disconnect_timed_out = true;

                    // Nếu kết nối mạng mới thất bại, không ghi đè và phục hồi mạng tốt từ NVS nếu có
                    if (was_attempting_new_save)
                    {
                        String nvs_ssid, nvs_pass;
                        unlock_wifi();
                        if (wifi_manager_load_credentials(nvs_ssid, nvs_pass) && nvs_ssid.length() > 0)
                        {
                            lock_wifi();
                            strlcpy(target_ssid, nvs_ssid.c_str(), sizeof(target_ssid));
                            strlcpy(target_pass, nvs_pass.c_str(), sizeof(target_pass));
                            log_i("Phục hồi mạng đã lưu [%s] sau khi kết nối mạng mới thất bại", target_ssid);
                        }
                        else
                        {
                            lock_wifi();
                        }
                    }
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
            const bool scan_blocks_reconnect = scan_control.busy() ||
                scan_driver.phase() != WifiScanDriverPhase::IDLE;
            const bool retry = !scan_blocks_reconnect && auto_reconnect_enabled &&
                               !manual_disconnect && strlen(target_ssid) > 0 &&
                               (millis() - last_disconnect_time >= reconnect_backoff_ms);
            if (retry)
            {
                log_i("Tự động kết nối lại WiFi '%s' (Backoff: %u ms)...", target_ssid, reconnect_backoff_ms);
                reconnect_backoff_ms = (reconnect_backoff_ms * 2 > 60000) ? 60000 : (reconnect_backoff_ms * 2);
                last_disconnect_time = millis();

                retry_cmd.type = WIFI_CMD_CONNECT;
                retry_cmd.generation = next_generation_locked();
                strlcpy(retry_cmd.ssid, target_ssid, sizeof(retry_cmd.ssid));
                strlcpy(retry_cmd.pass, target_pass, sizeof(retry_cmd.pass));
                if (wifi_retry_preserves_save(should_save_credentials, manual_disconnect, false))
                {
                    pending_save_generation = retry_cmd.generation;
                    retry_cmd.save_to_nvs = true;
                }
                else
                {
                    pending_save_generation = 0;
                    retry_cmd.save_to_nvs = false;
                }
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

        // Completion events, rather than scanComplete(), are authoritative.
        // Arduino's _scanDone() remains the only consumer of the IDF AP list.
        if (scan_driver.take_drained_event())
        {
            log_scan_driver_event(scan_driver_request_id, "drained", ESP_OK);
            scan_driver_request_id = 0;
        }

        const uint32_t now = millis();
        if (scan_driver.drain_expired(now, WIFI_SCAN_DRAIN_TIMEOUT_MS))
        {
            esp_err_t stop_error = ESP_FAIL;
            esp_err_t start_error = ESP_FAIL;
            const bool recovered = scan_driver.recover_radio(now, stop_error, start_error);
            log_scan_driver_event(scan_driver_request_id,
                                  recovered ? "recovered" : "recovery-failed",
                                  recovered ? ESP_OK : start_error);
            lock_wifi();
            if (current_state == WIFI_STATE_CONNECTED ||
                current_state == WIFI_STATE_CONNECTING)
            {
                current_state = WIFI_STATE_DISCONNECTED;
                active_connect_generation = 0;
                clear_connected_cache_locked();
                last_disconnect_time = now;
            }
            if (!recovered && scan_control.busy())
            {
                const uint32_t failed_id = scan_control.request_id;
                if (scan_control.finish(failed_id, WifiScanPhase::FAILED))
                {
                    set_scan_driver_error_locked("WiFi scan radio recovery failed", start_error);
                    log_scan_transition_locked("recovery-failed");
                }
            }
            unlock_wifi();
            scan_driver_request_id = 0;
            scan_retry_at_ms = now + WIFI_SCAN_RETRY_BACKOFF_MS;
        }

        lock_wifi();
        const WifiScanPhase scan_phase = scan_control.phase;
        const uint32_t scan_id = scan_control.request_id;
        const uint32_t scan_queued_at = scan_control.queued_at_ms;
        const WiFiState connection_state = current_state;
        unlock_wifi();

        if (scan_phase == WifiScanPhase::WAITING_FOR_RADIO)
        {
            if (wifi_scan_total_expired(now, scan_queued_at, WIFI_SCAN_TOTAL_BUDGET_MS))
            {
                lock_wifi();
                if (scan_control.finish(scan_id, WifiScanPhase::FAILED))
                {
                    set_scan_error_locked("WiFi scan exceeded total deadline");
                    scan_driver_error = ESP_ERR_TIMEOUT;
                    log_scan_transition_locked("total-timeout");
                }
                unlock_wifi();
            }
            else if (scan_driver.phase() == WifiScanDriverPhase::IDLE &&
                     static_cast<int32_t>(now - scan_retry_at_ms) >= 0 &&
                     wifi_scan_may_start(connection_state == WIFI_STATE_CONNECTING,
                                         now, scan_queued_at, WIFI_SCAN_CONNECT_WAIT_MS))
            {
                bool radio_ready = true;
                if (connection_state == WIFI_STATE_CONNECTING)
                {
                    radio_ready = WiFi.disconnect() || WiFi.status() != WL_CONNECTED;
                    if (radio_ready)
                    {
                        lock_wifi();
                        if (scan_id == scan_control.request_id &&
                            scan_control.phase == WifiScanPhase::WAITING_FOR_RADIO)
                        {
                            active_connect_generation = 0;
                            current_state = WIFI_STATE_DISCONNECTED;
                            last_disconnect_time = now;
                        }
                        unlock_wifi();
                    }
                }

                if (!radio_ready)
                {
                    lock_wifi();
                    if (scan_control.finish(scan_id, WifiScanPhase::FAILED))
                    {
                        set_scan_error_locked("Cannot stop WiFi connection attempt for scan");
                        scan_driver_error = ESP_ERR_WIFI_STATE;
                        log_scan_transition_locked("connect-cancel-failed");
                    }
                    unlock_wifi();
                }
                else if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 8192 ||
                         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 4096)
                {
                    lock_wifi();
                    if (scan_control.finish(scan_id, WifiScanPhase::FAILED))
                    {
                        set_scan_error_locked("Not enough memory for WiFi scan results");
                        scan_driver_error = ESP_ERR_NO_MEM;
                        log_scan_transition_locked("oom");
                    }
                    unlock_wifi();
                }
                else
                {
                    ++scan_start_attempts;
                    const WifiScanStartResult started = scan_driver.start(scan_id, now);
                    log_scan_driver_event(scan_id, started.accepted ? "accepted" : "start-failed",
                                          started.error);
                    if (started.accepted)
                    {
                        bool current = false;
                        lock_wifi();
                        if (scan_control.driver_accepted(scan_id, now, WIFI_SCAN_TIMEOUT_MS))
                        {
                            scan_driver_request_id = scan_id;
                            scan_driver_error = ESP_OK;
                            current = true;
                            log_scan_transition_locked("driver-accepted");
                        }
                        unlock_wifi();
                        if (!current)
                        {
                            scan_driver_request_id = scan_id;
                            const esp_err_t stop_error = scan_driver.stop(scan_id, millis());
                            log_scan_driver_event(scan_id, "stale-after-accept", stop_error);
                        }
                    }
                    else if (started.error == ESP_ERR_WIFI_STATE &&
                             scan_start_attempts < WIFI_SCAN_MAX_START_ATTEMPTS)
                    {
                        scan_retry_at_ms = now + WIFI_SCAN_RETRY_BACKOFF_MS;
                    }
                    else
                    {
                        lock_wifi();
                        if (scan_control.finish(scan_id, WifiScanPhase::FAILED))
                        {
                            set_scan_driver_error_locked("WiFi scan start failed", started.error);
                            log_scan_transition_locked("driver-rejected");
                        }
                        unlock_wifi();
                    }
                }
            }
        }

        lock_wifi();
        const bool current_driver_scan = wifi_scan_result_belongs_to(
            scan_driver_request_id, scan_control);
        const bool timed_out = scan_control.expired(scan_driver_request_id, millis());
        unlock_wifi();

        if (scan_driver_request_id && !current_driver_scan)
        {
            WifiScanDriverCompletion discarded = {};
            if (scan_driver.take_completion(scan_driver_request_id, discarded))
            {
                scan_driver.release_results(scan_driver_request_id);
                log_scan_driver_event(scan_driver_request_id, "stale-completion", ESP_OK);
                scan_driver_request_id = 0;
            }
            else if (scan_driver.phase() == WifiScanDriverPhase::RUNNING ||
                     scan_driver.phase() == WifiScanDriverPhase::STARTING)
            {
                const esp_err_t stop_error = scan_driver.stop(scan_driver_request_id, millis());
                log_scan_driver_event(scan_driver_request_id, "cancel-stop", stop_error);
            }
        }
        else if (current_driver_scan && timed_out)
        {
            const esp_err_t stop_error = scan_driver.stop(scan_driver_request_id, millis());
            lock_wifi();
            if (scan_control.finish(scan_driver_request_id, WifiScanPhase::FAILED))
            {
                set_scan_driver_error_locked("WiFi scan timed out", stop_error);
                log_scan_transition_locked("timeout");
            }
            unlock_wifi();
            log_scan_driver_event(scan_driver_request_id, "timeout-stop", stop_error);
        }
        else if (current_driver_scan)
        {
            WifiScanDriverCompletion completion = {};
            if (scan_driver.take_completion(scan_driver_request_id, completion))
            {
                bool result_ok = completion.status == 0;
                size_t copied = 0;
                const size_t requested = wifi_scan_bounded_result_count(
                    completion.result_count, WIFI_SCAN_MAX_RESULTS);
                memset(scan_copy_buffer, 0, sizeof(scan_copy_buffer));
                for (size_t i = 0; result_ok && i < requested; ++i)
                {
                    String ssid;
                    uint8_t encryption = WIFI_AUTH_OPEN;
                    int32_t rssi = 0;
                    uint8_t *bssid = nullptr;
                    int32_t channel = 0;
                    if (!WiFi.getNetworkInfo(static_cast<uint8_t>(i), ssid, encryption,
                                             rssi, bssid, channel))
                    {
                        result_ok = false;
                        break;
                    }
                    strlcpy(scan_copy_buffer[copied].ssid, ssid.c_str(),
                            sizeof(scan_copy_buffer[copied].ssid));
                    scan_copy_buffer[copied].rssi = rssi;
                    scan_copy_buffer[copied].channel = static_cast<uint8_t>(channel);
                    scan_copy_buffer[copied].is_encrypted = encryption != WIFI_AUTH_OPEN;
                    ++copied;
                }
                scan_driver.release_results(scan_driver_request_id);

                lock_wifi();
                if (wifi_scan_result_belongs_to(scan_driver_request_id, scan_control))
                {
                    if (result_ok)
                    {
                        memcpy(scan_results, scan_copy_buffer,
                               copied * sizeof(WiFiNetworkInfo));
                        scan_result_count = copied;
                        scan_error[0] = '\0';
                        scan_driver_error = ESP_OK;
                        scan_control.finish(scan_driver_request_id, WifiScanPhase::DONE);
                        log_scan_transition_locked("done");
                    }
                    else
                    {
                        scan_result_count = 0;
                        scan_driver_error = completion.status == 0 ? ESP_ERR_NO_MEM : ESP_FAIL;
                        if (completion.status == 0)
                            set_scan_error_locked("WiFi scan result buffer unavailable");
                        else
                            snprintf(scan_error, sizeof(scan_error),
                                     "WiFi scan event failed: status=%u", completion.status);
                        scan_control.finish(scan_driver_request_id, WifiScanPhase::FAILED);
                        log_scan_transition_locked("event-failed");
                    }
                }
                unlock_wifi();
                log_i("WiFi scan request=%u scan_id=%u status=%u reported=%u copied=%u elapsed=%u ms",
                      scan_driver_request_id, completion.scan_id,
                      static_cast<unsigned>(completion.status),
                      static_cast<unsigned>(completion.result_count),
                      static_cast<unsigned>(copied),
                      static_cast<unsigned>(millis() - scan_control.started_at_ms));
                scan_driver_request_id = 0;
                scan_start_attempts = 0;
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
        strlcpy(connection_error, "Cannot create WiFi mutex", sizeof(connection_error));
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
        strlcpy(connection_error, "Cannot create WiFi service task", sizeof(connection_error));
        log_e("WiFi degraded: không tạo được service task");
        return false;
    }
    return true;
}

bool wifi_manager_scan_async(void)
{
    if (!wifi_mutex || !wifi_command_queue) return false;
    WiFiCommand cmd = {};
    cmd.type = WIFI_CMD_SCAN;
    lock_wifi();
    if (scan_control.busy())
    {
        unlock_wifi();
        return false;
    }
    cmd.generation = scan_control.queue(millis());
    scan_result_count = 0;
    scan_error[0] = '\0';
    scan_driver_error = ESP_OK;
    scan_start_attempts = 0;
    scan_retry_at_ms = 0;
    log_scan_transition_locked("queued");
    unlock_wifi();
    if (!enqueue_wifi_command(cmd))
    {
        lock_wifi();
        if (scan_control.finish(cmd.generation, WifiScanPhase::FAILED))
        {
            set_scan_error_locked("WiFi scan command queue full");
            log_scan_transition_locked("queue-full");
        }
        unlock_wifi();
        return false;
    }
    return true;
}

bool wifi_manager_cancel_scan(void)
{
    if (!wifi_mutex) return false;
    lock_wifi();
    const uint32_t id = scan_control.request_id;
    const bool canceled = scan_control.finish(id, WifiScanPhase::CANCELED);
    if (canceled)
    {
        set_scan_error_locked("WiFi scan canceled");
        log_scan_transition_locked("cancel-requested");
    }
    unlock_wifi();
    if (canceled && wifi_task_handle) xTaskNotifyGive(wifi_task_handle);
    return canceled;
}

WiFiScanSnapshot wifi_manager_get_scan_snapshot(void)
{
    WiFiScanSnapshot snapshot = {};
    lock_wifi();
    snapshot.request_id = scan_control.request_id;
    snapshot.revision = scan_control.revision;
    snapshot.result_revision = scan_control.result_revision;
    snapshot.queued_at_ms = scan_control.queued_at_ms;
    snapshot.started_at_ms = scan_control.started_at_ms;
    snapshot.state = scan_control.phase;
    snapshot.driver_error = scan_driver_error;
    strlcpy(snapshot.error, scan_error, sizeof(snapshot.error));
    snapshot.result_count = scan_result_count;
    memcpy(snapshot.results, scan_results,
           scan_result_count * sizeof(WiFiNetworkInfo));
    unlock_wifi();
    return snapshot;
}

bool wifi_manager_is_scan_done(void)
{
    lock_wifi();
    const bool done = scan_control.phase == WifiScanPhase::IDLE ||
                      WifiScanCoordinator::terminal(scan_control.phase);
    unlock_wifi();
    return done;
}

String wifi_manager_get_last_error(void)
{
    lock_wifi();
    String error(connection_error);
    unlock_wifi();
    return error;
}

String wifi_manager_get_connection_error(void)
{
    return wifi_manager_get_last_error();
}

std::vector<WiFiNetworkInfo> wifi_manager_get_scan_results(void)
{
    lock_wifi();
    std::vector<WiFiNetworkInfo> res;
    res.reserve(scan_result_count);
    for (size_t i = 0; i < scan_result_count; ++i) res.push_back(scan_results[i]);
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
    uint32_t generation = request_generation + 1;
    if (generation == 0) ++generation;
    cmd.generation = generation;
    save_to_nvs = wifi_connect_may_save(save_to_nvs, pending_forget_control,
                                        current_state == WIFI_STATE_FORGETTING);
    cmd.save_to_nvs = save_to_nvs;
    // Commit desired connection state only after the queue accepts the command.
    // The worker also takes wifi_mutex before acting, so it cannot observe the
    // queued command before this transaction is published.
    if (!enqueue_wifi_command(cmd))
    {
        set_error_locked("WiFi command queue full");
        unlock_wifi();
        return false;
    }
    request_generation = generation;
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

    should_save_credentials = save_to_nvs; // Chỉ lưu NVS khi người dùng chủ động cấu hình
    pending_save_generation = save_to_nvs ? generation : 0;
    connection_error[0] = '\0';
    unlock_wifi();
    return true;
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
    pending_disconnect_command = cmd;
    pending_disconnect_control = true;
    control_status = WIFI_CONTROL_REQUESTED;
    unlock_wifi();
    if (wifi_task_handle) xTaskNotifyGive(wifi_task_handle);
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

bool wifi_manager_load_credentials(String &ssid, String &pass)
{
    if (!prefs_mutex) return false;
    lock_prefs();
    bool ok = prefs.begin(WIFI_PREFS_NAMESPACE, false);
    if (ok)
    {
        ssid = prefs.getString(WIFI_PREFS_KEY_SSID, "");
        pass = prefs.getString(WIFI_PREFS_KEY_PASS, "");
        if (ssid.length() > 0)
        {
            const uint32_t calc_chk = wifi_credentials_checksum(ssid.c_str(), pass.c_str());
            if (prefs.isKey(WIFI_PREFS_KEY_CHK))
            {
                const uint32_t chk = prefs.getUInt(WIFI_PREFS_KEY_CHK, 0);
                if (chk != 0 && chk == calc_chk)
                {
                    prefs.end();
                    unlock_prefs();
                    return true;
                }
                // Checksum không khớp hoặc bằng 0 -> Phát hiện ghi dở / mất nguồn giữa chừng!
                log_w("[WIFI] Checksum NVS chính không khớp. Kiểm tra bản sao lưu...");
                String bak_ssid = prefs.getString(WIFI_PREFS_KEY_BAK_SSID, "");
                String bak_pass = prefs.getString(WIFI_PREFS_KEY_BAK_PASS, "");
                uint32_t bak_chk = prefs.getUInt(WIFI_PREFS_KEY_BAK_CHK, 0);
                if (bak_ssid.length() > 0 && bak_chk != 0 &&
                    bak_chk == wifi_credentials_checksum(bak_ssid.c_str(), bak_pass.c_str()))
                {
                    log_i("[WIFI] Phục hồi cấu hình WiFi từ bản sao lưu: %s", bak_ssid.c_str());
                    prefs.putString(WIFI_PREFS_KEY_SSID, bak_ssid);
                    prefs.putString(WIFI_PREFS_KEY_PASS, bak_pass);
                    prefs.putUInt(WIFI_PREFS_KEY_CHK, bak_chk);
                    ssid = bak_ssid;
                    pass = bak_pass;
                    prefs.end();
                    unlock_prefs();
                    return true;
                }
                // Cả 2 slot đều không hợp lệ
                prefs.end();
                unlock_prefs();
                return false;
            }
            else
            {
                // Cấu hình cũ (legacy) chưa có key chk -> Tự động migrate có kiểm tra
                prefs.putUInt(WIFI_PREFS_KEY_CHK, calc_chk);
                prefs.end();
                unlock_prefs();
                return true;
            }
        }
        prefs.end();
    }
    unlock_prefs();
    return false;
}

bool wifi_manager_has_saved_credentials(void)
{
    String s, p;
    return wifi_manager_load_credentials(s, p);
}

bool wifi_manager_is_credentials_saved(void)
{
    lock_wifi();
    const bool connected = (current_state == WIFI_STATE_CONNECTED);
    char cur_ssid[33] = {0};
    strlcpy(cur_ssid, connected_ssid, sizeof(cur_ssid));
    unlock_wifi();

    if (!connected || strlen(cur_ssid) == 0) return false;

    String saved_s, saved_p;
    if (wifi_manager_load_credentials(saved_s, saved_p))
    {
        return (saved_s == cur_ssid);
    }
    return false;
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

bool wifi_manager_clear_credentials(void)
{
    if (!wifi_command_queue) return false;
    WiFiCommand cmd = {};
    cmd.type = WIFI_CMD_CLEAR_ONLY;
    return enqueue_wifi_command(cmd);
}

bool wifi_manager_forget_network(void)
{
    if (!wifi_task_handle || !wifi_mutex) return false;
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
    pending_forget_command = cmd;
    pending_forget_control = true;
    // Forget dominates an older Disconnect; both require the same radio stop,
    // and only Forget additionally commits credential deletion.
    pending_disconnect_control = false;
    control_status = WIFI_CONTROL_REQUESTED;
    unlock_wifi();
    xTaskNotifyGive(wifi_task_handle);
    return true;
}

WiFiControlStatus wifi_manager_get_control_status(void)
{
    lock_wifi();
    const WiFiControlStatus status = control_status;
    unlock_wifi();
    return status;
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
