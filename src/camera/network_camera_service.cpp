/**
 * @file network_camera_service.cpp
 * @brief Triển khai dịch vụ IP Camera qua mạng (HTTP Snapshot thật, ONVIF scaffold, bảo mật credential)
 */

#include "network_camera_service.h"
#include "../os/wifi_manager.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

NetworkCameraService g_network_camera;

#define NET_CAM_MAX_JPEG_BUF (96 * 1024) // 96 KB trong PSRAM cho Snapshot JPEG

NetworkCameraService::NetworkCameraService()
    : _configured(false), _connected(false), _running(false),
      _onvif_probed(false), _snapshot_jpeg_buf(nullptr), _snapshot_buf_size(NET_CAM_MAX_JPEG_BUF),
      _frame_mutex(nullptr), _worker_task_handle(nullptr),
      _snapshot_status(CAM_STATUS_NOT_IMPLEMENTED),
      _mjpeg_status(CAM_STATUS_NOT_IMPLEMENTED),
      _rtsp_status(CAM_STATUS_NOT_IMPLEMENTED),
      _onvif_status(CAM_STATUS_NOT_IMPLEMENTED)
{
    memset(&_profile, 0, sizeof(_profile));
    memset(&_current_frame, 0, sizeof(_current_frame));
    _onvif_snapshot_url[0] = '\0';
    _onvif_stream_url[0] = '\0';
}

NetworkCameraService::~NetworkCameraService()
{
    stop();
    if (_snapshot_jpeg_buf)
    {
        free(_snapshot_jpeg_buf);
        _snapshot_jpeg_buf = nullptr;
    }
    if (_frame_mutex)
    {
        vSemaphoreDelete(_frame_mutex);
        _frame_mutex = nullptr;
    }
}

void NetworkCameraService::urlEncode(const char *src, char *dst, size_t dst_len)
{
    if (!src || !dst || dst_len == 0) return;
    static const char hex[] = "0123456789ABCDEF";
    size_t out_idx = 0;

    for (size_t i = 0; src[i] != '\0' && out_idx + 3 < dst_len; i++)
    {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            dst[out_idx++] = c;
        }
        else
        {
            dst[out_idx++] = '%';
            dst[out_idx++] = hex[(c >> 4) & 0x0F];
            dst[out_idx++] = hex[c & 0x0F];
        }
    }
    dst[out_idx] = '\0';
}

bool NetworkCameraService::configure(const NetworkCameraProfile &profile)
{
    _profile = profile;
    _configured = (strlen(_profile.ip) > 0);
    _connected = false;

    // Bảo mật: Tuyệt đối không log username/password dạng plaintext ra Serial
    Serial.printf("[NET_CAM] Cấu hình IP Camera: [%s] Hãng: %s IP: %s:%u Ch:%u (Credentials Masked)\n",
                  _profile.name, getVendorName(_profile.vendor), _profile.ip, _profile.port, _profile.channel);

    if (_frame_mutex == nullptr)
    {
        _frame_mutex = xSemaphoreCreateMutex();
    }

    // Cập nhật trạng thái tính năng theo protocol được chọn
    switch (_profile.protocol)
    {
        case CAM_PROTO_HTTP_SNAPSHOT:
            _snapshot_status = CAM_STATUS_READY;
            _mjpeg_status = CAM_STATUS_NOT_IMPLEMENTED;
            _rtsp_status = CAM_STATUS_NOT_IMPLEMENTED;
            break;
        case CAM_PROTO_MJPEG:
            _snapshot_status = CAM_STATUS_NOT_IMPLEMENTED;
            _mjpeg_status = CAM_STATUS_NOT_IMPLEMENTED;
            _rtsp_status = CAM_STATUS_NOT_IMPLEMENTED;
            break;
        case CAM_PROTO_RTSP:
            _snapshot_status = CAM_STATUS_NOT_IMPLEMENTED;
            _mjpeg_status = CAM_STATUS_NOT_IMPLEMENTED;
            _rtsp_status = CAM_STATUS_NOT_IMPLEMENTED;
            break;
    }

    _onvif_status = CAM_STATUS_PARTIAL_FALLBACK;
    return _configured;
}

void NetworkCameraService::workerTaskEntry(void *param)
{
    NetworkCameraService *self = (NetworkCameraService *)param;
    if (self) self->workerTask();
}

void NetworkCameraService::workerTask()
{
    Serial.println("[NET_CAM] 🚀 Worker Task nạp HTTP Snapshot bắt đầu trên Core 0");

    while (_running)
    {
        if (wifi_manager_is_connected() && _configured)
        {
            if (_snapshot_jpeg_buf == nullptr)
            {
                if (psramFound())
                {
                    _snapshot_jpeg_buf = (uint8_t *)heap_caps_malloc(_snapshot_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (!_snapshot_jpeg_buf)
                {
                    _snapshot_jpeg_buf = (uint8_t *)malloc(_snapshot_buf_size);
                }
            }

            if (_snapshot_jpeg_buf)
            {
                int bytes = fetchHttpSnapshot(_snapshot_jpeg_buf, _snapshot_buf_size);
                if (bytes > 200)
                {
                    if (_frame_mutex && xSemaphoreTake(_frame_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
                    {
                        _current_frame.buf = _snapshot_jpeg_buf;
                        _current_frame.len = (size_t)bytes;
                        _current_frame.width = 480;
                        _current_frame.height = 320;
                        _current_frame.format = CAM_PIXFORMAT_JPEG;
                        _current_frame.timestamp_ms = millis();
                        xSemaphoreGive(_frame_mutex);
                    }
                    _connected = true;
                    _snapshot_status = CAM_STATUS_READY;
                }
                else
                {
                    _connected = false;
                    _snapshot_status = CAM_STATUS_ERROR;
                }
            }
        }
        else
        {
            _connected = false;
        }

        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    _worker_task_handle = nullptr;
    vTaskDelete(NULL);
}

bool NetworkCameraService::start()
{
    if (!_configured)
    {
        Serial.println("[NET_CAM] ❌ Chưa cấu hình thông số IP Camera.");
        return false;
    }

    if (_profile.protocol == CAM_PROTO_HTTP_SNAPSHOT)
    {
        _running = true;
        if (_worker_task_handle == nullptr)
        {
            xTaskCreatePinnedToCore(
                workerTaskEntry,
                "NetCamWorker",
                4096,
                this,
                2,
                &_worker_task_handle,
                0 // Chạy trên Core 0
            );
        }
        char masked_url[256];
        buildStreamUrl(masked_url, sizeof(masked_url), true);
        Serial.printf("[NET_CAM] Bắt đầu dịch vụ HTTP Snapshot: %s\n", masked_url);
        return true;
    }
    else if (_profile.protocol == CAM_PROTO_MJPEG)
    {
        _connected = false;
        _mjpeg_status = CAM_STATUS_NOT_IMPLEMENTED;
        Serial.println("[NET_CAM] ⚠️ Giao thức MJPEG chưa hoàn chỉnh -> Đánh dấu NOT_IMPLEMENTED");
        return false;
    }
    else if (_profile.protocol == CAM_PROTO_RTSP)
    {
        _connected = false;
        _rtsp_status = CAM_STATUS_NOT_IMPLEMENTED;
        Serial.println("[NET_CAM] ⚠️ Giao thức RTSP/H.264 chưa hoàn chỉnh -> Đánh dấu NOT_IMPLEMENTED");
        return false;
    }

    return false;
}

void NetworkCameraService::stop()
{
    _running = false;
    _connected = false;
    Serial.println("[NET_CAM] ⏹ Đã dừng dịch vụ IP Camera.");
}

bool NetworkCameraService::isConnected() const
{
    return _connected;
}

const NetworkCameraProfile& NetworkCameraService::getActiveProfile() const
{
    return _profile;
}

CameraFeatureStatus NetworkCameraService::getSnapshotStatus() const
{
    return _snapshot_status;
}

CameraFeatureStatus NetworkCameraService::getMjpegStatus() const
{
    return _mjpeg_status;
}

CameraFeatureStatus NetworkCameraService::getRtspStatus() const
{
    return _rtsp_status;
}

CameraFeatureStatus NetworkCameraService::getOnvifStatus() const
{
    return _onvif_status;
}

CameraFrame* NetworkCameraService::getFrame(uint32_t timeout_ms)
{
    if (!_connected || _current_frame.buf == nullptr || _current_frame.len == 0)
    {
        return nullptr;
    }
    return &_current_frame;
}

void NetworkCameraService::returnFrame(CameraFrame *frame)
{
    // Dữ liệu bộ đệm được quản lý nội bộ trong PSRAM
}

void NetworkCameraService::buildStreamUrl(char *out_url, size_t max_len, bool mask_credential) const
{
    if (!out_url || max_len == 0) return;

    if (strlen(_profile.custom_url) > 0)
    {
        strncpy(out_url, _profile.custom_url, max_len - 1);
        out_url[max_len - 1] = '\0';
        return;
    }

    uint8_t ch = _profile.channel > 0 ? _profile.channel : 1;
    uint16_t port = _profile.port > 0 ? _profile.port : 554;

    char encoded_user[48] = {0};
    char encoded_pass[48] = {0};
    if (mask_credential)
    {
        strcpy(encoded_user, "***");
        strcpy(encoded_pass, "***");
    }
    else
    {
        urlEncode(_profile.username, encoded_user, sizeof(encoded_user));
        urlEncode(_profile.password, encoded_pass, sizeof(encoded_pass));
    }

    switch (_profile.vendor)
    {
        case CAM_VENDOR_HIKVISION:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/Streaming/Channels/%u01",
                     encoded_user, encoded_pass, _profile.ip, port, ch);
            break;

        case CAM_VENDOR_KBVISION:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/cam/realmonitor?channel=%u&subtype=0",
                     encoded_user, encoded_pass, _profile.ip, port, ch);
            break;

        case CAM_VENDOR_EZVIZ:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/h264/ch%u/main/av_stream",
                     encoded_user, encoded_pass, _profile.ip, port, ch);
            break;

        case CAM_VENDOR_YOOSEE:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/onvif1",
                     encoded_user, encoded_pass, _profile.ip, port);
            break;

        case CAM_VENDOR_GENERIC_ONVIF:
        default:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/live/ch%u",
                     encoded_user, encoded_pass, _profile.ip, port, ch);
            break;
    }
}

void NetworkCameraService::buildSnapshotUrl(char *out_url, size_t max_len) const
{
    if (!out_url || max_len == 0) return;

    if (strlen(_onvif_snapshot_url) > 0)
    {
        strncpy(out_url, _onvif_snapshot_url, max_len - 1);
        out_url[max_len - 1] = '\0';
        return;
    }

    uint8_t ch = _profile.channel > 0 ? _profile.channel : 1;
    uint16_t http_port = (_profile.port == 554 || _profile.port == 0) ? 80 : _profile.port;

    switch (_profile.vendor)
    {
        case CAM_VENDOR_HIKVISION:
            snprintf(out_url, max_len, "http://%s:%u/ISAPI/Streaming/channels/%u01/picture",
                     _profile.ip, http_port, ch);
            break;

        case CAM_VENDOR_KBVISION:
            snprintf(out_url, max_len, "http://%s:%u/cgi-bin/snapshot.cgi?channel=%u",
                     _profile.ip, http_port, ch);
            break;

        case CAM_VENDOR_EZVIZ:
        case CAM_VENDOR_YOOSEE:
        case CAM_VENDOR_GENERIC_ONVIF:
        default:
            snprintf(out_url, max_len, "http://%s:%u/onvif/snapshot",
                     _profile.ip, http_port);
            break;
    }
}

bool NetworkCameraService::onvifProbeCapabilities(char *out_service_url, size_t max_len)
{
    if (!_configured || !wifi_manager_is_connected()) return false;

    uint16_t http_port = (_profile.port == 554 || _profile.port == 0) ? 80 : _profile.port;
    char probe_url[128];
    snprintf(probe_url, sizeof(probe_url), "http://%s:%u/onvif/device_service", _profile.ip, http_port);

    HTTPClient http;
    WiFiClient client;
    http.begin(client, probe_url);
    http.setTimeout(2500);
    http.addHeader("Content-Type", "application/soap+xml; charset=utf-8");

    const char *soap_req =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body><tds:GetCapabilities xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/></s:Body>"
        "</s:Envelope>";

    int httpCode = http.POST(soap_req);
    if (httpCode == 200)
    {
        _onvif_status = CAM_STATUS_READY;
        _onvif_probed = true;
        if (out_service_url && max_len > 0)
        {
            strncpy(out_service_url, probe_url, max_len - 1);
            out_service_url[max_len - 1] = '\0';
        }
        http.end();
        return true;
    }

    _onvif_status = CAM_STATUS_PARTIAL_FALLBACK;
    http.end();
    return false;
}

bool NetworkCameraService::onvifGetProfiles(char *out_profile_token, size_t max_len)
{
    if (out_profile_token && max_len > 0)
    {
        strncpy(out_profile_token, "Profile_1", max_len - 1);
        out_profile_token[max_len - 1] = '\0';
    }
    return true;
}

bool NetworkCameraService::onvifGetSnapshotUri(const char *profile_token, char *out_uri, size_t max_len)
{
    buildSnapshotUrl(out_uri, max_len);
    return true;
}

bool NetworkCameraService::onvifGetStreamUri(const char *profile_token, char *out_uri, size_t max_len)
{
    buildStreamUrl(out_uri, max_len, false);
    return true;
}

int NetworkCameraService::fetchHttpSnapshot(uint8_t *out_buf, size_t max_size)
{
    if (!_configured || !out_buf || max_size == 0) return -1;

    char url[256];
    buildSnapshotUrl(url, sizeof(url));

    HTTPClient http;
    WiFiClient client;
    if (strlen(_profile.username) > 0)
    {
        http.setAuthorization(_profile.username, _profile.password);
    }
    http.begin(client, url);
    http.setTimeout(4000);

    int httpCode = http.GET();
    int bytesRead = -1;

    if (httpCode == HTTP_CODE_OK)
    {
        int len = http.getSize();
        if (len > 0 && (size_t)len <= max_size)
        {
            WiFiClient *stream = http.getStreamPtr();
            bytesRead = stream->readBytes(out_buf, len);
        }
    }
    http.end();
    return bytesRead;
}

const char* NetworkCameraService::getVendorName(CameraVendorProfile vendor)
{
    switch (vendor)
    {
        case CAM_VENDOR_HIKVISION: return "Hikvision";
        case CAM_VENDOR_KBVISION:  return "KBVision / Dahua";
        case CAM_VENDOR_EZVIZ:     return "Ezviz";
        case CAM_VENDOR_YOOSEE:    return "Yoosee";
        case CAM_VENDOR_GENERIC_ONVIF:
        default:                   return "ONVIF Generic";
    }
}

