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

#define NET_CAM_DEFAULT_BUF_CAP (256 * 1024) // 256 KB trong PSRAM cho Snapshot JPEG
#define NET_CAM_MAX_SAFETY_LIMIT (512 * 1024) // Giới hạn an toàn tối đa 512 KB

NetworkCameraService::NetworkCameraService()
    : _configured(false), _connected(false), _running(false),
      _onvif_probed(false),
      _buf_front(nullptr), _buf_back(nullptr), _buf_capacity(NET_CAM_DEFAULT_BUF_CAP),
      _front_in_use(false), _frame_mutex(nullptr), _worker_task_handle(nullptr),
      _snapshot_status(CAM_STATUS_NOT_IMPLEMENTED),
      _mjpeg_status(CAM_STATUS_NOT_IMPLEMENTED),
      _rtsp_status(CAM_STATUS_NOT_IMPLEMENTED),
      _onvif_status(CAM_STATUS_NOT_IMPLEMENTED)
{
    memset(&_profile, 0, sizeof(_profile));
    memset(&_frame_front, 0, sizeof(_frame_front));
    memset(&_frame_back, 0, sizeof(_frame_back));
    _onvif_snapshot_url[0] = '\0';
    _onvif_stream_url[0] = '\0';
}

NetworkCameraService::~NetworkCameraService()
{
    stop();
    if (_buf_front)
    {
        free(_buf_front);
        _buf_front = nullptr;
    }
    if (_buf_back)
    {
        free(_buf_back);
        _buf_back = nullptr;
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

void NetworkCameraService::sanitizeUrl(const char *src, char *dst, size_t dst_len)
{
    if (!src || !dst || dst_len == 0) return;
    const char *scheme_end = strstr(src, "://");
    const char *at_sign = strchr(src, '@');

    if (scheme_end && at_sign && at_sign > scheme_end + 3)
    {
        // Có credential nằm giữa :// và @ -> Mask thành ***:***
        size_t scheme_len = (scheme_end - src) + 3;
        if (scheme_len < dst_len)
        {
            strncpy(dst, src, scheme_len);
            dst[scheme_len] = '\0';
            strncat(dst, "***:***", dst_len - strlen(dst) - 1);
            strncat(dst, at_sign, dst_len - strlen(dst) - 1);
            return;
        }
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

bool NetworkCameraService::parseJpegDimensions(const uint8_t *buf, size_t len, size_t &width, size_t &height)
{
    width = 0;
    height = 0;
    if (!buf || len < 4) return false;
    if (buf[0] != 0xFF || buf[1] != 0xD8) return false; // SOI kiểm tra

    size_t idx = 2;
    while (idx + 4 < len)
    {
        if (buf[idx] != 0xFF)
        {
            idx++;
            continue;
        }
        uint8_t marker = buf[idx + 1];
        // Bỏ qua padding 0xFF
        if (marker == 0xFF || marker == 0x00)
        {
            idx += 2;
            continue;
        }
        // SOF0 (Baseline), SOF1 (Extended), SOF2 (Progressive)
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2)
        {
            if (idx + 9 < len)
            {
                height = (buf[idx + 5] << 8) | buf[idx + 6];
                width  = (buf[idx + 7] << 8) | buf[idx + 8];
                return (width > 0 && height > 0);
            }
            return false;
        }
        // SOS (Start of Scan) hoặc EOI (End of Image) -> kết thúc phần header
        if (marker == 0xDA || marker == 0xD9)
        {
            break;
        }
        // Độ dài phân đoạn marker
        uint16_t block_len = (buf[idx + 2] << 8) | buf[idx + 3];
        if (block_len < 2 || idx + 2 + block_len > len)
        {
            break;
        }
        idx += 2 + block_len;
    }
    return false;
}

bool NetworkCameraService::configure(const NetworkCameraProfile &profile)
{
    _profile = profile;
    _configured = (strlen(_profile.ip) > 0 || strlen(_profile.custom_url) > 0);
    _connected = false;

    // Bảo mật: Tuyệt đối không log username/password dạng plaintext ra Serial
    uint16_t h_port = _profile.http_port > 0 ? _profile.http_port : 80;
    uint16_t r_port = _profile.rtsp_port > 0 ? _profile.rtsp_port : 554;
    Serial.printf("[NET_CAM] Cấu hình IP Camera: [%s] Hãng: %s IP: %s (HTTP:%u, RTSP:%u, ONVIF:%u) Ch:%u [Credentials Protected]\n",
                  _profile.name, getVendorName(_profile.vendor), _profile.ip, h_port, r_port, _profile.onvif_port, _profile.channel);

    if (_frame_mutex == nullptr)
    {
        _frame_mutex = xSemaphoreCreateMutex();
    }

    // Cập nhật trạng thái tính năng theo protocol được chọn
    switch (_profile.protocol)
    {
        case CAM_PROTO_HTTP_SNAPSHOT:
            // Chỉ đánh dấu READY sau khi luồng fetch bắt được frame thành công
            _snapshot_status = CAM_STATUS_PARTIAL_FALLBACK;
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

    _onvif_status = CAM_STATUS_NOT_IMPLEMENTED;
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
            // Cấp phát ping-pong double buffer trong PSRAM nếu chưa có
            if (_buf_front == nullptr)
            {
                if (psramFound())
                {
                    _buf_front = (uint8_t *)heap_caps_malloc(_buf_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (!_buf_front) _buf_front = (uint8_t *)malloc(_buf_capacity);
            }
            if (_buf_back == nullptr)
            {
                if (psramFound())
                {
                    _buf_back = (uint8_t *)heap_caps_malloc(_buf_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (!_buf_back) _buf_back = (uint8_t *)malloc(_buf_capacity);
            }

            if (_buf_back && _buf_front)
            {
                int bytes = fetchHttpSnapshot(_buf_back, _buf_capacity);
                if (bytes >= 4 && _buf_back[0] == 0xFF && _buf_back[1] == 0xD8)
                {
                    // Trích xuất kích thước thực tế từ JPEG Header (SOF marker)
                    size_t real_w = 0, real_h = 0;
                    bool has_dim = parseJpegDimensions(_buf_back, (size_t)bytes, real_w, real_h);
                    if (!has_dim)
                    {
                        real_w = 320;
                        real_h = 240;
                    }

                    // Hoán đổi atomic back buffer sang front buffer an toàn
                    if (_frame_mutex && xSemaphoreTake(_frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        if (!_front_in_use)
                        {
                            uint8_t *tmp = _buf_front;
                            _buf_front = _buf_back;
                            _buf_back = tmp;

                            _frame_front.buf = _buf_front;
                            _frame_front.len = (size_t)bytes;
                            _frame_front.width = real_w;
                            _frame_front.height = real_h;
                            _frame_front.format = CAM_PIXFORMAT_JPEG;
                            _frame_front.timestamp_ms = millis();

                            _connected = true;
                            _snapshot_status = CAM_STATUS_READY;
                        }
                        xSemaphoreGive(_frame_mutex);
                    }
                }
                else
                {
                    if (_snapshot_status != CAM_STATUS_READY)
                    {
                        _connected = false;
                        _snapshot_status = CAM_STATUS_ERROR;
                    }
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
        buildSnapshotUrl(masked_url, sizeof(masked_url));
        char sanitized_log[256];
        sanitizeUrl(masked_url, sanitized_log, sizeof(sanitized_log));
        Serial.printf("[NET_CAM] Bắt đầu dịch vụ HTTP Snapshot: %s\n", sanitized_log);
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
    if (!_connected || _frame_front.buf == nullptr || _frame_front.len == 0)
    {
        return nullptr;
    }
    if (_frame_mutex && xSemaphoreTake(_frame_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        _front_in_use = true;
        return &_frame_front;
    }
    return nullptr;
}

void NetworkCameraService::returnFrame(CameraFrame *frame)
{
    if (frame && _front_in_use && _frame_mutex)
    {
        _front_in_use = false;
        xSemaphoreGive(_frame_mutex);
    }
}

void NetworkCameraService::buildStreamUrl(char *out_url, size_t max_len, bool mask_credential) const
{
    if (!out_url || max_len == 0) return;

    if (strlen(_profile.custom_url) > 0)
    {
        if (mask_credential)
        {
            sanitizeUrl(_profile.custom_url, out_url, max_len);
        }
        else
        {
            strncpy(out_url, _profile.custom_url, max_len - 1);
            out_url[max_len - 1] = '\0';
        }
        return;
    }

    uint8_t ch = _profile.channel > 0 ? _profile.channel : 1;
    uint16_t port = _profile.rtsp_port > 0 ? _profile.rtsp_port : 554;

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
    uint16_t http_port = _profile.http_port > 0 ? _profile.http_port : 80;

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

    uint16_t onvif_port = _profile.onvif_port > 0 ? _profile.onvif_port : 80;
    char probe_url[128];
    snprintf(probe_url, sizeof(probe_url), "http://%s:%u/onvif/device_service", _profile.ip, onvif_port);

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
        _onvif_status = CAM_STATUS_PARTIAL_FALLBACK;
        _onvif_probed = true;
        if (out_service_url && max_len > 0)
        {
            strncpy(out_service_url, probe_url, max_len - 1);
            out_service_url[max_len - 1] = '\0';
        }
        http.end();
        return true;
    }

    _onvif_status = CAM_STATUS_NOT_IMPLEMENTED;
    http.end();
    return false;
}

bool NetworkCameraService::onvifGetProfiles(char *out_profile_token, size_t max_len)
{
    // Chưa parse SOAP XML GetProfiles thật -> không trả success giả
    _onvif_status = CAM_STATUS_NOT_IMPLEMENTED;
    return false;
}

bool NetworkCameraService::onvifGetSnapshotUri(const char *profile_token, char *out_uri, size_t max_len)
{
    // Chưa parse SOAP XML GetSnapshotUri thật -> không trả success giả
    _onvif_status = CAM_STATUS_NOT_IMPLEMENTED;
    return false;
}

bool NetworkCameraService::onvifGetStreamUri(const char *profile_token, char *out_uri, size_t max_len)
{
    // Chưa parse SOAP XML GetStreamUri thật -> không trả success giả
    _onvif_status = CAM_STATUS_NOT_IMPLEMENTED;
    return false;
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
        int expected_len = http.getSize();
        WiFiClient *stream = http.getStreamPtr();
        if (stream)
        {
            size_t total = 0;
            uint32_t start_ms = millis();
            // Đọc an toàn hỗ trợ cả Content-Length > 0 và chunked/stream (expected_len == -1)
            while ((http.connected() || stream->available()) && (millis() - start_ms < 4000))
            {
                size_t avail = stream->available();
                if (avail > 0)
                {
                    size_t to_read = avail;
                    if (total + to_read > max_size)
                    {
                        to_read = max_size - total;
                    }
                    if (to_read == 0) break; // Đạt giới hạn an toàn max buffer

                    int r = stream->readBytes(out_buf + total, to_read);
                    if (r > 0)
                    {
                        total += r;
                        // Kiểm tra nếu đã nhận đủ marker EOI kết thúc ảnh JPEG (0xFF, 0xD9)
                        if (total >= 4 && out_buf[total - 2] == 0xFF && out_buf[total - 1] == 0xD9)
                        {
                            break;
                        }
                    }
                }
                else
                {
                    if (expected_len > 0 && total >= (size_t)expected_len) break;
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            if (total > 0)
            {
                bytesRead = (int)total;
            }
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


