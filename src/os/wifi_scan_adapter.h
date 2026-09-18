#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_err.h>
#include "wifi_scan_adapter_logic.h"

struct WifiScanStartResult
{
    esp_err_t error = ESP_FAIL;
    bool accepted = false;
};

class WifiScanDriverAdapter
{
public:
    bool begin();
    WifiScanStartResult start(uint32_t request_id, uint32_t now_ms);
    esp_err_t stop(uint32_t request_id, uint32_t now_ms);
    bool take_completion(uint32_t request_id, WifiScanDriverCompletion &completion);
    bool take_drained_event();
    bool drain_expired(uint32_t now_ms, uint32_t timeout_ms);
    bool recover_radio(uint32_t now_ms, esp_err_t &stop_error, esp_err_t &start_error);
    void release_results(uint32_t request_id);
    WifiScanDriverPhase phase();
    uint32_t stale_event_count();

private:
    static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info);
    static WifiScanDriverAdapter *instance_;
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    WifiScanAdapterLogic logic_;
    bool initialized_ = false;
};

