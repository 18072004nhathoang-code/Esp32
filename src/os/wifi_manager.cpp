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
static uint32_t scan_driver_request_id = 0;
static bool scan_driver_draining = false;
static uint32_t scan_drain_deadline_ms = 0;
static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 15000;
static constexpr uint32_t WIFI_SCAN_CONNECT_WAIT_MS = 4000;

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
    log_i("WiFi scan request=%u event=%s state=%s queued=%u start=%u conn=%d",
          scan_control.request_id, event, scan_phase_text(scan_control.phase),
          scan_control.queued_at_ms, scan_control.started_at_ms, static_cast<int>(current_state));
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
    log_i("WiFi connect generation=%u SSID=%s", command.generation, command.ssid);
    WiFi.begin(command.ssid, command.pass);
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
                const esp_err_t stop_err = esp_wifi_scan_stop();
                scan_driver_draining = true;
                scan_drain_deadline_ms = millis() + 2000;
                log_i("WiFi scan request=%u stop=%d", scan_driver_request_id, static_cast<int>(stop_err));
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
                const bool scan_busy = scan_control.busy() || scan_driver_draining;
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
                                      !scan_driver_draining;
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
                    connection_error[0] = '\0';
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
            const bool scan_blocks_reconnect = scan_control.busy() || scan_driver_draining;
            const bool retry = !scan_blocks_reconnect && auto_reconnect_enabled &&
                               !manual_disconnect && strlen(target_ssid) > 0 &&
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

        // Drain a stopped driver's late completion before another request is
        // allowed to own the radio. scanDelete() is cleanup only, never cancel.
        if (scan_driver_draining)
        {
            const int late = WiFi.scanComplete();
            if (late != WIFI_SCAN_RUNNING)
            {
                WiFi.scanDelete();
                log_i("WiFi scan request=%u late-completion=%d discarded",
                      scan_driver_request_id, late);
                scan_driver_request_id = 0;
                scan_driver_draining = false;
                scan_drain_deadline_ms = 0;
            }
            else if (static_cast<int32_t>(millis() - scan_drain_deadline_ms) >= 0)
            {
                // A wedged late scan must not starve every later manual scan.
                // Reset only the STA radio under its sole worker owner.
                log_e("WiFi scan request=%u drain timeout; resetting STA radio",
                      scan_driver_request_id);
                WiFi.disconnect();
                WiFi.mode(WIFI_OFF);
                vTaskDelay(pdMS_TO_TICKS(50));
                WiFi.mode(WIFI_STA);
                WiFi.setAutoReconnect(false);
                WiFi.scanDelete();
                lock_wifi();
                if (current_state == WIFI_STATE_CONNECTED ||
                    current_state == WIFI_STATE_CONNECTING)
                {
                    current_state = WIFI_STATE_DISCONNECTED;
                    clear_connected_cache_locked();
                    last_disconnect_time = millis();
                }
                unlock_wifi();
                scan_driver_request_id = 0;
                scan_driver_draining = false;
                scan_drain_deadline_ms = 0;
            }
        }

        lock_wifi();
        const WifiScanPhase scan_phase = scan_control.phase;
        const uint32_t scan_id = scan_control.request_id;
        const uint32_t scan_queued_at = scan_control.queued_at_ms;
        const WiFiState connection_state = current_state;
        unlock_wifi();

        if (scan_phase == WifiScanPhase::WAITING_FOR_RADIO && !scan_driver_draining)
        {
            const uint32_t now = millis();
            const bool connecting = connection_state == WIFI_STATE_CONNECTING;
            if (wifi_scan_may_start(connecting, now, scan_queued_at,
                                    WIFI_SCAN_CONNECT_WAIT_MS))
            {
                if (connecting)
                {
                    WiFi.disconnect();
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

                if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 8192)
                {
                    lock_wifi();
                    if (scan_control.finish(scan_id, WifiScanPhase::FAILED))
                    {
                        set_scan_error_locked("Not enough memory for WiFi scan results");
                        log_scan_transition_locked("oom");
                    }
                    unlock_wifi();
                }
                else
                {
                    // Background scan keeps an established STA connection up.
                    const int accepted = WiFi.scanNetworks(true, true);
                    if (accepted == WIFI_SCAN_FAILED)
                    {
                        lock_wifi();
                        if (scan_control.finish(scan_id, WifiScanPhase::FAILED))
                        {
                            set_scan_error_locked("WiFi driver rejected scan");
                            log_scan_transition_locked("driver-rejected");
                        }
                        unlock_wifi();
                    }
                    else
                    {
                        bool accepted_is_current = false;
                        lock_wifi();
                        if (scan_control.driver_accepted(scan_id, now, WIFI_SCAN_TIMEOUT_MS))
                        {
                            scan_driver_request_id = scan_id;
                            accepted_is_current = true;
                            log_scan_transition_locked("driver-accepted");
                        }
                        unlock_wifi();
                        if (!accepted_is_current)
                        {
                            // Cancellation may race the async driver call. The
                            // accepted scan is now an old request and must be
                            // stopped/drained before a newer scan can start.
                            scan_driver_request_id = scan_id;
                            const esp_err_t stop_err = esp_wifi_scan_stop();
                            scan_driver_draining = true;
                            scan_drain_deadline_ms = millis() + 2000;
                            log_i("WiFi scan request=%u stale-after-accept stop=%d",
                                  scan_id, static_cast<int>(stop_err));
                        }
                    }
                }
            }
        }

        lock_wifi();
        const bool current_driver_scan = wifi_scan_result_belongs_to(
            scan_driver_request_id, scan_control);
        const bool timed_out = scan_control.expired(scan_driver_request_id, millis());
        unlock_wifi();
        if (scan_driver_request_id && !current_driver_scan && !scan_driver_draining)
        {
            const esp_err_t stop_err = esp_wifi_scan_stop();
            scan_driver_draining = true;
            scan_drain_deadline_ms = millis() + 2000;
            log_i("WiFi scan request=%u canceled stop=%d",
                  scan_driver_request_id, static_cast<int>(stop_err));
        }
        else if (current_driver_scan && timed_out)
        {
            const esp_err_t stop_err = esp_wifi_scan_stop();
            lock_wifi();
            if (scan_control.finish(scan_driver_request_id, WifiScanPhase::FAILED))
            {
                set_scan_error_locked("WiFi scan timed out");
                log_scan_transition_locked("timeout");
            }
            unlock_wifi();
            scan_driver_draining = true;
            scan_drain_deadline_ms = millis() + 2000;
            log_w("WiFi scan request=%u timeout stop=%d",
                  scan_driver_request_id, static_cast<int>(stop_err));
        }
        else if (current_driver_scan)
        {
            const int n = WiFi.scanComplete();
            if (n >= 0)
            {
                const size_t count = static_cast<size_t>(n) > WIFI_SCAN_MAX_RESULTS
                    ? WIFI_SCAN_MAX_RESULTS : static_cast<size_t>(n);
                WiFiNetworkInfo completed[WIFI_SCAN_MAX_RESULTS] = {};
                for (size_t i = 0; i < count; ++i)
                {
                    strlcpy(completed[i].ssid, WiFi.SSID(static_cast<int>(i)).c_str(),
                            sizeof(completed[i].ssid));
                    completed[i].rssi = WiFi.RSSI(static_cast<int>(i));
                    completed[i].channel = WiFi.channel(static_cast<int>(i));
                    completed[i].is_encrypted =
                        WiFi.encryptionType(static_cast<int>(i)) != WIFI_AUTH_OPEN;
                }
                WiFi.scanDelete();
                lock_wifi();
                if (wifi_scan_result_belongs_to(scan_driver_request_id, scan_control))
                {
                    memcpy(scan_results, completed, count * sizeof(WiFiNetworkInfo));
                    scan_result_count = count;
                    scan_error[0] = '\0';
                    scan_control.finish(scan_driver_request_id, WifiScanPhase::DONE);
                    log_scan_transition_locked("done");
                    log_i("WiFi scan request=%u copied=%u driver_results=%d",
                          scan_driver_request_id, static_cast<unsigned>(count), n);
                }
                unlock_wifi();
                scan_driver_request_id = 0;
            }
            else if (n == WIFI_SCAN_FAILED)
            {
                WiFi.scanDelete();
                lock_wifi();
                if (scan_control.finish(scan_driver_request_id, WifiScanPhase::FAILED))
                {
                    set_scan_error_locked("WiFi scan failed");
                    log_scan_transition_locked("failed");
                    log_e("WiFi scan request=%u driver_result=%d",
                          scan_driver_request_id, n);
                }
                unlock_wifi();
                scan_driver_request_id = 0;
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

    current_state = WIFI_STATE_CONNECTING;
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
