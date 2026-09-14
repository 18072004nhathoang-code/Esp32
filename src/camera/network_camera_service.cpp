/**
 * @file network_camera_service.cpp
 * @brief Triển khai dịch vụ IP Camera qua mạng (Hikvision, KBVision, Ezviz, Yoosee, ONVIF)
 */

#include "network_camera_service.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <stdio.h>
#include <string.h>

NetworkCameraService g_network_camera;

NetworkCameraService::NetworkCameraService()
    : _configured(false), _connected(false)
{
    memset(&_profile, 0, sizeof(_profile));
}

NetworkCameraService::~NetworkCameraService()
{
    stop();
}

bool NetworkCameraService::configure(const NetworkCameraProfile &profile)
{
    _profile = profile;
    _configured = (strlen(_profile.ip) > 0);
    Serial.printf("[NET_CAM] Đã cấu hình IP Camera: [%s] Hãng: %s IP: %s:%u\n",
                  _profile.name, getVendorName(_profile.vendor), _profile.ip, _profile.port);
    return _configured;
}

bool NetworkCameraService::start()
{
    if (!_configured)
    {
        Serial.println("[NET_CAM] Chưa cấu hình thông số IP Camera.");
        return false;
    }
    _connected = true;
    char url[256];
    buildStreamUrl(url, sizeof(url));
    Serial.printf("[NET_CAM] Đã khởi chạy luồng IP Camera: %s\n", url);
    return true;
}

void NetworkCameraService::stop()
{
    _connected = false;
    Serial.println("[NET_CAM] Đã dừng luồng kết nối IP Camera.");
}

bool NetworkCameraService::isConnected() const
{
    return _connected;
}

const NetworkCameraProfile& NetworkCameraService::getActiveProfile() const
{
    return _profile;
}

void NetworkCameraService::buildStreamUrl(char *out_url, size_t max_len) const
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

    switch (_profile.vendor)
    {
        case CAM_VENDOR_HIKVISION:
            // Hikvision RTSP format: rtsp://user:pass@ip:port/Streaming/Channels/{ch}01
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/Streaming/Channels/%u01",
                     _profile.username, _profile.password, _profile.ip, port, ch);
            break;

        case CAM_VENDOR_KBVISION:
            // KBVision / Dahua RTSP format: rtsp://user:pass@ip:port/cam/realmonitor?channel={ch}&subtype=0
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/cam/realmonitor?channel=%u&subtype=0",
                     _profile.username, _profile.password, _profile.ip, port, ch);
            break;

        case CAM_VENDOR_EZVIZ:
            // Ezviz local RTSP format: rtsp://admin:verification_code@ip:554/h264/ch1/main/av_stream
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/h264/ch%u/main/av_stream",
                     _profile.username, _profile.password, _profile.ip, port, ch);
            break;

        case CAM_VENDOR_YOOSEE:
            // Yoosee / SriHome RTSP: rtsp://admin:pass@ip:554/onvif1
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/onvif1",
                     _profile.username, _profile.password, _profile.ip, port);
            break;

        case CAM_VENDOR_GENERIC_ONVIF:
        default:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/live/ch%u",
                     _profile.username, _profile.password, _profile.ip, port, ch);
            break;
    }
}

void NetworkCameraService::buildSnapshotUrl(char *out_url, size_t max_len) const
{
    if (!out_url || max_len == 0) return;

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
