#include "xiaozhi_activation.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef XIAOZHI_OTA_ENDPOINT
#define XIAOZHI_OTA_ENDPOINT "https://api.tenclass.net/xiaozhi/ota/"
#endif
#ifndef XIAOZHI_OTA_CA_CERT
#define XIAOZHI_OTA_CA_CERT ""
#endif

namespace
{
static const char *kNamespace = "xiaozhi";
static const size_t kProvisionBodyLimit = 16U * 1024U;

class BoundedSink : public Stream
{
public:
    explicit BoundedSink(size_t capacity)
        : data_(static_cast<uint8_t *>(heap_caps_malloc(capacity + 1,
              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))), size_(0), capacity_(capacity), failed_(false)
    {
        if (!data_) data_ = static_cast<uint8_t *>(malloc(capacity + 1));
        if (!data_) failed_ = true;
    }
    ~BoundedSink() override { free(data_); }
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t *buffer, size_t length) override
    {
        if (!buffer || failed_ || length > capacity_ - size_)
        {
            failed_ = true;
            return 0;
        }
        memcpy(data_ + size_, buffer, length);
        size_ += length;
        data_[size_] = 0;
        return length;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    const uint8_t *data() const { return data_; }
    size_t size() const { return size_; }
    bool failed() const { return failed_; }
private:
    uint8_t *data_;
    size_t size_;
    size_t capacity_;
    bool failed_;
};

void make_uuid_v4(char *out, size_t size)
{
    uint8_t bytes[16];
    for (size_t i = 0; i < sizeof(bytes); i += 4)
    {
        const uint32_t value = esp_random();
        memcpy(bytes + i, &value, sizeof(value));
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    snprintf(out, size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

bool save_config(const xiaozhi::ProvisionedWebsocket &config)
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    const bool ok = prefs.putString("ws_url", config.url) > 0 &&
                    prefs.putString("ws_token", config.token) > 0 &&
                    prefs.putUChar("ws_ver", config.version) == 1;
    prefs.end();
    return ok;
}

bool copy_json_string(JsonVariantConst value, char *out, size_t size)
{
    if (!out || size == 0 || !value.is<const char *>()) return false;
    const char *text = value.as<const char *>();
    if (!text || !*text || strlen(text) >= size) return false;
    strlcpy(out, text, size);
    return true;
}
}

bool xiaozhi_identity_init(char *device_id, size_t device_id_size,
                           char *client_id, size_t client_id_size)
{
    if (!device_id || device_id_size < 18 || !client_id || client_id_size < 37) return false;
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
    snprintf(device_id, device_id_size, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    String stored = prefs.getString("client_id", "");
    if (stored.length() != 36)
    {
        char generated[37] = {};
        make_uuid_v4(generated, sizeof(generated));
        if (prefs.putString("client_id", generated) != 36)
        {
            prefs.end();
            return false;
        }
        stored = generated;
    }
    prefs.end();
    strlcpy(client_id, stored.c_str(), client_id_size);
    return true;
}

bool xiaozhi_load_websocket_config(xiaozhi::ProvisionedWebsocket *config)
{
    if (!config) return false;
    memset(config, 0, sizeof(*config));
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false;
    const String url = prefs.getString("ws_url", "");
    const String token = prefs.getString("ws_token", "");
    config->version = prefs.getUChar("ws_ver", 1);
    prefs.end();
    if (url.length() >= sizeof(config->url) || token.length() >= sizeof(config->token)) return false;
    strlcpy(config->url, url.c_str(), sizeof(config->url));
    strlcpy(config->token, token.c_str(), sizeof(config->token));
    return xiaozhi::valid_websocket_config(*config);
}

bool xiaozhi_provision_once(XiaozhiProvisionResult *result,
                            char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (!result) return false;
    memset(result, 0, sizeof(*result));
    result->poll_after_ms = 10000;
    if (strncmp(XIAOZHI_OTA_ENDPOINT, "https://", 8) != 0 || XIAOZHI_OTA_CA_CERT[0] == '\0')
    {
        if (error && error_size) strlcpy(error, "Cần cấu hình Xiaozhi OTA CA hợp lệ", error_size);
        return false;
    }

    char device_id[18] = {};
    char client_id[37] = {};
    if (!xiaozhi_identity_init(device_id, sizeof(device_id), client_id, sizeof(client_id)))
    {
        if (error && error_size) strlcpy(error, "Không tạo được định danh Xiaozhi trong NVS", error_size);
        return false;
    }

    StaticJsonDocument<512> info;
    info["version"] = 2;
    info["language"] = "vi-VN";
    info["flash_size"] = ESP.getFlashChipSize();
    info["psram_size"] = ESP.getPsramSize();
    info["minimum_free_heap_size"] = ESP.getMinFreeHeap();
    info["mac_address"] = device_id;
    info["uuid"] = client_id;
    info["chip_model_name"] = ESP.getChipModel();
    JsonObject application = info.createNestedObject("application");
    application["name"] = "esp32-mini-os";
#ifdef FW_GIT_SHA
    application["version"] = FW_GIT_SHA;
#else
    application["version"] = "unknown";
#endif
    String request_body;
    serializeJson(info, request_body);

    WiFiClientSecure tls;
    tls.setCACert(XIAOZHI_OTA_CA_CERT);
    HTTPClient http;
    if (!http.begin(tls, XIAOZHI_OTA_ENDPOINT))
    {
        if (error && error_size) strlcpy(error, "Không mở được Xiaozhi OTA HTTPS", error_size);
        return false;
    }
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    http.addHeader("Activation-Version", "1");
    http.addHeader("Device-Id", device_id);
    http.addHeader("Client-Id", client_id);
    http.addHeader("User-Agent", "ES3C28P-MiniOS/1");
    http.addHeader("Accept-Language", "vi-VN");
    http.addHeader("Content-Type", "application/json");
    const int status = http.POST(request_body);
    const int announced_size = http.getSize();
    if (status != HTTP_CODE_OK || announced_size > static_cast<int>(kProvisionBodyLimit))
    {
        if (error && error_size)
            snprintf(error, error_size, status > 0 ? "Xiaozhi OTA HTTP %d" : "Xiaozhi OTA TLS/network lỗi", status);
        http.end();
        return false;
    }
    BoundedSink sink(kProvisionBodyLimit);
    const int received = http.writeToStream(&sink);
    http.end();
    if (sink.failed() || received < 0 || static_cast<size_t>(received) != sink.size() ||
        (announced_size >= 0 && static_cast<size_t>(announced_size) != sink.size()))
    {
        if (error && error_size) strlcpy(error, "Xiaozhi OTA trả dữ liệu lỗi/quá giới hạn", error_size);
        return false;
    }

    DynamicJsonDocument document(8192);
    if (deserializeJson(document, sink.data(), sink.size(), DeserializationOption::NestingLimit(8)) ||
        !document.is<JsonObject>())
    {
        if (error && error_size) strlcpy(error, "Xiaozhi OTA JSON không hợp lệ", error_size);
        return false;
    }
    JsonObjectConst root = document.as<JsonObjectConst>();
    JsonObjectConst activation = root["activation"].as<JsonObjectConst>();
    if (!activation.isNull())
    {
        copy_json_string(activation["code"], result->activation_code,
                         sizeof(result->activation_code));
        copy_json_string(activation["message"], result->activation_message,
                         sizeof(result->activation_message));
        const uint32_t timeout_ms = activation["timeout_ms"] | 10000U;
        result->poll_after_ms = xiaozhi::clamp_activation_poll_ms(timeout_ms);
        result->activation_required = result->activation_code[0] != '\0';
    }

    JsonObjectConst websocket = root["websocket"].as<JsonObjectConst>();
    if (!websocket.isNull())
    {
        xiaozhi::ProvisionedWebsocket config = {};
        const bool url_ok = copy_json_string(websocket["url"], config.url, sizeof(config.url));
        const bool token_ok = copy_json_string(websocket["token"], config.token, sizeof(config.token));
        const int version = websocket["version"] | 1;
        config.version = static_cast<uint8_t>(version);
        if (!url_ok || !token_ok || !xiaozhi::valid_websocket_config(config))
        {
            if (error && error_size) strlcpy(error, "Xiaozhi WebSocket config không hợp lệ", error_size);
            return false;
        }
        if (!save_config(config))
        {
            if (error && error_size) strlcpy(error, "Không lưu được Xiaozhi WebSocket config", error_size);
            return false;
        }
        result->configured = true;
        result->websocket = config;
        result->activation_required = false;
    }

    // Firmware/assets fields are deliberately ignored: this integration never
    // downloads or applies upstream OTA payloads.
    return result->configured || result->activation_required;
}
