#include "wifi_scan_adapter.h"

#include <esp_wifi.h>

WifiScanDriverAdapter *WifiScanDriverAdapter::instance_ = nullptr;

bool WifiScanDriverAdapter::begin()
{
    if (initialized_) return true;
    instance_ = this;
    WiFi.onEvent(on_wifi_event, ARDUINO_EVENT_WIFI_SCAN_DONE);
    initialized_ = true;
    return true;
}

WifiScanStartResult WifiScanDriverAdapter::start(uint32_t request_id, uint32_t now_ms)
{
    WifiScanStartResult result;
    if (!initialized_)
    {
        result.error = ESP_ERR_INVALID_STATE;
        return result;
    }

    portENTER_CRITICAL(&mux_);
    const bool prepared = logic_.prepare_start(request_id, now_ms);
    portEXIT_CRITICAL(&mux_);
    if (!prepared)
    {
        result.error = ESP_ERR_WIFI_STATE;
        return result;
    }

    // Arduino remains the sole owner of the IDF result list through _scanDone().
    // This only releases a previously consumed result buffer before a new scan.
    WiFi.scanDelete();

    wifi_scan_config_t config;
    wifi_scan_zero_config(config);
    const WifiScanSettings settings;
    config.ssid = nullptr;
    config.bssid = nullptr;
    config.channel = 0;
    config.show_hidden = settings.show_hidden;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = settings.active_min_ms;
    config.scan_time.active.max = settings.active_max_ms;
    config.home_chan_dwell_time = settings.home_channel_dwell_ms;

    result.error = esp_wifi_scan_start(&config, false);
    const bool may_be_busy = result.error == ESP_ERR_WIFI_STATE;
    portENTER_CRITICAL(&mux_);
    result.accepted = logic_.start_returned(
        request_id, static_cast<int32_t>(result.error), may_be_busy, millis());
    portEXIT_CRITICAL(&mux_);
    return result;
}

esp_err_t WifiScanDriverAdapter::stop(uint32_t request_id, uint32_t now_ms)
{
    portENTER_CRITICAL(&mux_);
    const bool stopping = logic_.begin_stop(request_id, now_ms);
    portEXIT_CRITICAL(&mux_);
    if (!stopping) return ESP_ERR_INVALID_STATE;

    const esp_err_t error = esp_wifi_scan_stop();
    // ESP_OK means the stop was submitted and its event must be drained.
    // ESP_ERR_WIFI_STATE is also ambiguous: an event can already be queued.
    const bool may_be_busy = error == ESP_OK || error == ESP_ERR_WIFI_STATE;
    portENTER_CRITICAL(&mux_);
    logic_.stop_returned(request_id, may_be_busy, millis());
    portEXIT_CRITICAL(&mux_);
    return error;
}

bool WifiScanDriverAdapter::take_completion(
    uint32_t request_id, WifiScanDriverCompletion &completion)
{
    portENTER_CRITICAL(&mux_);
    const bool ready = logic_.take_completion(request_id, completion);
    portEXIT_CRITICAL(&mux_);
    return ready;
}

bool WifiScanDriverAdapter::take_drained_event()
{
    portENTER_CRITICAL(&mux_);
    const bool ready = logic_.take_drained_event();
    portEXIT_CRITICAL(&mux_);
    if (ready) WiFi.scanDelete();
    return ready;
}

bool WifiScanDriverAdapter::drain_expired(uint32_t now_ms, uint32_t timeout_ms)
{
    portENTER_CRITICAL(&mux_);
    const bool expired = logic_.drain_expired(now_ms, timeout_ms);
    portEXIT_CRITICAL(&mux_);
    return expired;
}

bool WifiScanDriverAdapter::recover_radio(
    uint32_t now_ms, esp_err_t &stop_error, esp_err_t &start_error)
{
    portENTER_CRITICAL(&mux_);
    const bool recovering = logic_.begin_recovery(now_ms);
    portEXIT_CRITICAL(&mux_);
    if (!recovering)
    {
        stop_error = ESP_ERR_INVALID_STATE;
        start_error = ESP_ERR_INVALID_STATE;
        return false;
    }

    stop_error = esp_wifi_stop();
    start_error = (stop_error == ESP_OK || stop_error == ESP_ERR_WIFI_NOT_STARTED)
        ? esp_wifi_start() : ESP_ERR_INVALID_STATE;
    const bool success = (stop_error == ESP_OK || stop_error == ESP_ERR_WIFI_NOT_STARTED) &&
                         start_error == ESP_OK;
    if (success) WiFi.setAutoReconnect(false);
    WiFi.scanDelete();

    portENTER_CRITICAL(&mux_);
    logic_.recovery_finished(success, now_ms);
    portEXIT_CRITICAL(&mux_);
    return success;
}

void WifiScanDriverAdapter::release_results(uint32_t request_id)
{
    WiFi.scanDelete();
    portENTER_CRITICAL(&mux_);
    logic_.release_completion(request_id);
    portEXIT_CRITICAL(&mux_);
}

WifiScanDriverPhase WifiScanDriverAdapter::phase()
{
    portENTER_CRITICAL(&mux_);
    const WifiScanDriverPhase value = logic_.phase;
    portEXIT_CRITICAL(&mux_);
    return value;
}

uint32_t WifiScanDriverAdapter::stale_event_count()
{
    portENTER_CRITICAL(&mux_);
    const uint32_t value = logic_.stale_event_count;
    portEXIT_CRITICAL(&mux_);
    return value;
}

void WifiScanDriverAdapter::on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (event != ARDUINO_EVENT_WIFI_SCAN_DONE || !instance_) return;
    const wifi_event_sta_scan_done_t &done = info.wifi_scan_done;
    portENTER_CRITICAL(&instance_->mux_);
    instance_->logic_.on_scan_done(done.scan_id, done.status, done.number);
    portEXIT_CRITICAL(&instance_->mux_);
}

