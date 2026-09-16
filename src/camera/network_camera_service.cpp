/**
 * @file network_camera_service.cpp
 * @brief Triển khai dịch vụ IP Camera qua mạng (HTTP Snapshot thật, ONVIF scaffold, bảo mật credential)
 */

#include "network_camera_service.h"
#include "../os/wifi_manager.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

NetworkCameraService g_network_camera;

#define NET_CAM_DEFAULT_BUF_CAP (256 * 1024) // 256 KB trong PSRAM cho Snapshot JPEG
#define NET_CAM_MAX_SAFETY_LIMIT (512 * 1024) // Giới hạn an toàn tối đa 512 KB

#include <utility>

NetworkCameraService::NetworkCameraService()
    : _configured(false), _connected(false), _running(false),
      _runtime_state(CAM_STATE_NOT_CONFIGURED), _frame_sequence(0),
      _transport_security(CAM_TRANSPORT_NONE),
      _config_mutex(nullptr), _worker_exit_sem(nullptr),
      _onvif_probed(false),
      _buf_front(nullptr), _buf_back(nullptr),
      _front_capacity(NET_CAM_DEFAULT_BUF_CAP), _back_capacity(NET_CAM_DEFAULT_BUF_CAP),
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
    _config_mutex = xSemaphoreCreateMutex();
    _worker_exit_sem = xSemaphoreCreateBinary();
}

bool NetworkCameraService::ensureSynchronizationPrimitives()
{
    if (!_config_mutex) _config_mutex = xSemaphoreCreateMutex();
    if (!_worker_exit_sem) _worker_exit_sem = xSemaphoreCreateBinary();
    if (!_frame_mutex) _frame_mutex = xSemaphoreCreateMutex();
    if (!_config_mutex || !_worker_exit_sem || !_frame_mutex)
    {
        _runtime_state = CAM_STATE_ERROR;
        Serial.println("[NET_CAM] ❌ Chế độ suy giảm: không tạo được mutex/semaphore");
        return false;
    }
    return true;
}

void NetworkCameraService::setTransportSecurity(CameraTransportSecurity security)
{
    if (_transport_security == security) return;
    _transport_security = security;
    if (security == CAM_TRANSPORT_HTTP_PLAINTEXT)
    {
        Serial.println("[NET_CAM] ⚠️ CẢNH BÁO BẢO MẬT: HTTP plaintext; tài khoản và mật khẩu camera có thể bị nghe lén.");
    }
    else if (security == CAM_TRANSPORT_HTTPS_UNVERIFIED)
    {
        Serial.println("[NET_CAM] ⚠️ HTTPS đang mã hóa nhưng chứng chỉ camera chưa được xác thực; vẫn có rủi ro MITM.");
    }
}

NetworkCameraService::~NetworkCameraService()
{
    if (!stop(2000)) return;
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
    if (_config_mutex)
    {
        vSemaphoreDelete(_config_mutex);
        _config_mutex = nullptr;
    }
    if (_worker_exit_sem)
    {
        vSemaphoreDelete(_worker_exit_sem);
        _worker_exit_sem = nullptr;
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
    if (!ensureSynchronizationPrimitives()) return false;
    // Dừng worker cũ và chờ xác nhận thoát hoàn toàn trước khi đổi cấu hình
    if (!stop(2000))
    {
        Serial.println("[NET_CAM] Worker chưa thoát; giữ nguyên profile hiện tại.");
        return false;
    }

    if (xSemaphoreTake(_config_mutex, portMAX_DELAY) == pdTRUE)
    {
        _profile = profile;
        _configured = (strlen(_profile.ip) > 0 || strlen(_profile.custom_url) > 0);
        _connected = false;
        if (strlen(_profile.username) > 0 && strlen(_profile.password) == 0)
        {
            _runtime_state = CAM_STATE_PASSWORD_REQUIRED;
        }
        else
        {
            _runtime_state = _configured ? CAM_STATE_STOPPED : CAM_STATE_NOT_CONFIGURED;
        }
        xSemaphoreGive(_config_mutex);
    }

    // Bảo mật: Tuyệt đối không log username/password dạng plaintext ra Serial
    uint16_t h_port = _profile.http_port > 0 ? _profile.http_port : 80;
    uint16_t r_port = _profile.rtsp_port > 0 ? _profile.rtsp_port : 554;
    Serial.printf("[NET_CAM] Cấu hình IP Camera: [%s] Hãng: %s IP: %s (HTTP:%u, RTSP:%u, ONVIF:%u) Ch:%u [Mật khẩu chỉ giữ trong RAM; ưu tiên HTTPS]\n",
                  _profile.name, getVendorName(_profile.vendor), _profile.ip, h_port, r_port, _profile.onvif_port, _profile.channel);

    // Cập nhật trạng thái tính năng theo protocol được chọn
    switch (_profile.protocol)
    {
        case CAM_PROTO_HTTP_SNAPSHOT:
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

CameraRuntimeState NetworkCameraService::getRuntimeState() const
{
    return _runtime_state;
}

CameraTransportSecurity NetworkCameraService::getTransportSecurity() const
{
    return _transport_security;
}

bool NetworkCameraService::saveProfileToNVS()
{
    Preferences prefs;
    if (!prefs.begin("netcam", false)) return false;

    NetworkCameraProfile prof = getActiveProfile();
    bool saved = true;
    saved = (prefs.putString("name", prof.name) > 0) && saved;
    saved = (prefs.putString("ip", prof.ip) > 0) && saved;
    saved = (prefs.putUShort("http_port", prof.http_port) > 0) && saved;
    saved = (prefs.putUShort("rtsp_port", prof.rtsp_port) > 0) && saved;
    saved = (prefs.putUShort("onvif_port", prof.onvif_port) > 0) && saved;
    saved = (prefs.putUChar("vendor", (uint8_t)prof.vendor) > 0) && saved;
    saved = (prefs.putUChar("proto", (uint8_t)prof.protocol) > 0) && saved;
    saved = (prefs.putUChar("ch", prof.channel) > 0) && saved;
    saved = (prefs.putString("user", prof.username) > 0) && saved;
    // Lưu ý bảo mật: Mật khẩu không bao giờ được lưu plaintext vào Flash
    prefs.end();
    if (saved) Serial.println("[NET_CAM] ✔ Đã lưu cấu hình Camera vào NVS (mật khẩu không được lưu).");
    else Serial.println("[NET_CAM] ❌ Không thể lưu đầy đủ cấu hình Camera vào NVS.");
    return saved;
}

bool NetworkCameraService::loadProfileFromNVS()
{
    if (!ensureSynchronizationPrimitives()) return false;
    Preferences prefs;
    if (!prefs.begin("netcam", true)) return false;

    String ip = prefs.getString("ip", "");
    if (ip.length() == 0)
    {
        prefs.end();
        return false;
    }

    NetworkCameraProfile prof;
    memset(&prof, 0, sizeof(prof));

    String name = prefs.getString("name", "IP Cam");
    strncpy(prof.name, name.c_str(), sizeof(prof.name) - 1);
    strncpy(prof.ip, ip.c_str(), sizeof(prof.ip) - 1);
    prof.http_port = prefs.getUShort("http_port", 80);
    prof.rtsp_port = prefs.getUShort("rtsp_port", 554);
    prof.onvif_port = prefs.getUShort("onvif_port", 8000);
    prof.vendor = (CameraVendorProfile)prefs.getUChar("vendor", (uint8_t)CAM_VENDOR_GENERIC_ONVIF);
    prof.protocol = (CameraStreamProtocol)prefs.getUChar("proto", (uint8_t)CAM_PROTO_HTTP_SNAPSHOT);
    prof.channel = prefs.getUChar("ch", 1);
    String user = prefs.getString("user", "admin");
    strncpy(prof.username, user.c_str(), sizeof(prof.username) - 1);
    prof.password[0] = '\0'; // Mật khẩu không lưu để bảo mật

    prefs.end();

    if (xSemaphoreTake(_config_mutex, portMAX_DELAY) == pdTRUE)
    {
        _profile = prof;
        _configured = true;
        if (strlen(_profile.username) > 0 && strlen(_profile.password) == 0)
        {
            _runtime_state = CAM_STATE_PASSWORD_REQUIRED;
        }
        else
        {
            _runtime_state = CAM_STATE_STOPPED;
        }
        xSemaphoreGive(_config_mutex);
    }

    Serial.printf("[NET_CAM] ✔ Đã nạp cấu hình IP Camera từ NVS: %s (%s) [Password Required: %s]\n",
                  _profile.name, _profile.ip, (_runtime_state == CAM_STATE_PASSWORD_REQUIRED) ? "YES" : "NO");
    return true;
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
        NetworkCameraProfile cur_prof;
        bool is_cfg = false;
        CameraRuntimeState cur_state = CAM_STATE_NOT_CONFIGURED;

        if (_config_mutex && xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            cur_prof = _profile;
            is_cfg = _configured;
            cur_state = _runtime_state;
            xSemaphoreGive(_config_mutex);
        }
        else
        {
            cur_prof = _profile;
            is_cfg = _configured;
            cur_state = _runtime_state;
        }

        // Không tự động thực hiện request nếu thiếu password
        if (cur_state == CAM_STATE_PASSWORD_REQUIRED || (strlen(cur_prof.username) > 0 && strlen(cur_prof.password) == 0))
        {
            _connected = false;
            _runtime_state = CAM_STATE_PASSWORD_REQUIRED;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (wifi_manager_is_connected() && is_cfg)
        {
            // Cấp phát ping-pong double buffer trong PSRAM nếu chưa có với dung lượng độc lập
            if (_buf_front == nullptr)
            {
                if (psramFound())
                {
                    _buf_front = (uint8_t *)heap_caps_malloc(_front_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (!_buf_front) _buf_front = (uint8_t *)malloc(_front_capacity);
            }
            if (_buf_back == nullptr)
            {
                if (psramFound())
                {
                    _buf_back = (uint8_t *)heap_caps_malloc(_back_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (!_buf_back) _buf_back = (uint8_t *)malloc(_back_capacity);
            }

            if (_buf_back && _buf_front)
            {
                // Exactly one immutable profile snapshot is used for this request.
                const NetworkCameraProfile request_profile = cur_prof;
                int bytes = fetchHttpSnapshotForProfile(_buf_back, _back_capacity, request_profile);
                // Xác thực nghiêm ngặt: bytes >= 4, SOI = 0xFF 0xD8, EOI = 0xFF 0xD9
                if (bytes >= 4 &&
                    _buf_back[0] == 0xFF && _buf_back[1] == 0xD8 &&
                    _buf_back[bytes - 2] == 0xFF && _buf_back[bytes - 1] == 0xD9)
                {
                    // Trích xuất kích thước thực tế từ JPEG Header (SOF marker)
                    size_t real_w = 0, real_h = 0;
                    bool has_dim = parseJpegDimensions(_buf_back, (size_t)bytes, real_w, real_h);
                    if (!has_dim)
                    {
                        real_w = 320;
                        real_h = 240;
                    }

                    // Hoán đổi atomic back buffer sang front buffer kèm dung lượng thật
                    if (_frame_mutex && xSemaphoreTake(_frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        if (!_front_in_use)
                        {
                            std::swap(_buf_front, _buf_back);
                            std::swap(_front_capacity, _back_capacity);

                            _frame_sequence++;
                            _frame_front.buf = _buf_front;
                            _frame_front.len = (size_t)bytes;
                            _frame_front.width = real_w;
                            _frame_front.height = real_h;
                            _frame_front.format = CAM_PIXFORMAT_JPEG;
                            _frame_front.timestamp_ms = millis();
                            _frame_front.frame_id = _frame_sequence;

                            _connected = true;
                            _runtime_state = CAM_STATE_CONNECTED;
                            _snapshot_status = CAM_STATUS_READY;
                        }
                        xSemaphoreGive(_frame_mutex);
                    }
                }
                else
                {
                    _connected = false;
                    _runtime_state = CAM_STATE_ERROR;
                    if (_snapshot_status != CAM_STATUS_READY)
                    {
                        _snapshot_status = CAM_STATUS_ERROR;
                    }
                }
            }
        }
        else
        {
            _connected = false;
            if (!is_cfg)
            {
                _runtime_state = CAM_STATE_NOT_CONFIGURED;
            }
        }

        // Delay 1500ms nhưng kiểm tra _running mỗi 50ms để dừng tức thì khi stop() được gọi
        for (int i = 0; i < 30 && _running; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    _worker_task_handle = nullptr;
    if (_worker_exit_sem)
    {
        xSemaphoreGive(_worker_exit_sem);
    }
    vTaskDelete(NULL);
}

bool NetworkCameraService::start()
{
    if (!ensureSynchronizationPrimitives()) return false;
    NetworkCameraProfile request_profile = getActiveProfile();
    if (!_configured)
    {
        Serial.println("[NET_CAM] ❌ Chưa cấu hình thông số IP Camera.");
        _runtime_state = CAM_STATE_NOT_CONFIGURED;
        return false;
    }

    if (strlen(request_profile.username) > 0 && strlen(request_profile.password) == 0)
    {
        Serial.println("[NET_CAM] ⚠️ Cần nhập mật khẩu camera để xác thực!");
        _runtime_state = CAM_STATE_PASSWORD_REQUIRED;
        return false;
    }

    if (request_profile.protocol == CAM_PROTO_HTTP_SNAPSHOT)
    {
        if (_running && _worker_task_handle != nullptr)
        {
            return true;
        }
        if (_worker_exit_sem == nullptr)
        {
            _worker_exit_sem = xSemaphoreCreateBinary();
        }
        if (!_worker_exit_sem)
        {
            _runtime_state = CAM_STATE_ERROR;
            Serial.println("[NET_CAM] ❌ Chế độ suy giảm: không tạo được semaphore worker");
            return false;
        }
        xSemaphoreTake(_worker_exit_sem, 0); // Xóa token cũ nếu có

        _running = true;
        _runtime_state = CAM_STATE_CONNECTING;
        BaseType_t ret = xTaskCreatePinnedToCore(
            workerTaskEntry,
            "NetCamWorker",
            4096,
            this,
            2,
            &_worker_task_handle,
            0 // Chạy trên Core 0
        );
        if (ret != pdPASS)
        {
            _running = false;
            _runtime_state = CAM_STATE_ERROR;
            Serial.println("[NET_CAM] ❌ Không thể tạo NetCamWorker task!");
            return false;
        }
        char masked_url[256];
        buildSnapshotUrl(masked_url, sizeof(masked_url));
        char sanitized_log[256];
        sanitizeUrl(masked_url, sanitized_log, sizeof(sanitized_log));
        Serial.printf("[NET_CAM] Bắt đầu dịch vụ HTTP Snapshot: %s\n", sanitized_log);
        return true;
    }
    else if (request_profile.protocol == CAM_PROTO_MJPEG)
    {
        _connected = false;
        _runtime_state = CAM_STATE_ERROR;
        _mjpeg_status = CAM_STATUS_NOT_IMPLEMENTED;
        Serial.println("[NET_CAM] ⚠️ Giao thức MJPEG chưa hoàn chỉnh -> Đánh dấu NOT_IMPLEMENTED");
        return false;
    }
    else if (request_profile.protocol == CAM_PROTO_RTSP)
    {
        _connected = false;
        _runtime_state = CAM_STATE_ERROR;
        _rtsp_status = CAM_STATUS_NOT_IMPLEMENTED;
        Serial.println("[NET_CAM] ⚠️ Giao thức RTSP/H.264 chưa hoàn chỉnh -> Đánh dấu NOT_IMPLEMENTED");
        return false;
    }

    return false;
}

bool NetworkCameraService::stop(uint32_t timeout_ms)
{
    _running = false;
    uint32_t wait_start = millis();
    if (_worker_task_handle != nullptr)
    {
        if (_worker_exit_sem)
        {
            xSemaphoreTake(_worker_exit_sem, pdMS_TO_TICKS(timeout_ms));
        }
        while (_worker_task_handle != nullptr && (millis() - wait_start < timeout_ms))
        {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (_worker_task_handle != nullptr)
    {
        Serial.println("[NET_CAM] Worker stop timeout; tài nguyên vẫn được giữ nguyên.");
        return false;
    }
    _connected = false;
    if (_runtime_state != CAM_STATE_PASSWORD_REQUIRED)
    {
        _runtime_state = CAM_STATE_STOPPED;
    }
    Serial.println("[NET_CAM] ⏹ Đã dừng dịch vụ IP Camera.");
    return true;
}

bool NetworkCameraService::isConnected() const
{
    return _connected;
}

NetworkCameraProfile NetworkCameraService::getActiveProfile()
{
    NetworkCameraProfile prof;
    memset(&prof, 0, sizeof(prof));
    if (_config_mutex && xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        prof = _profile;
        xSemaphoreGive(_config_mutex);
    }
    else
    {
        prof = _profile;
    }
    return prof;
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

    if (_profile.custom_url[0] != '\0')
    {
        strncpy(out_url, _profile.custom_url, max_len - 1);
        out_url[max_len - 1] = '\0';
        return;
    }

    if (strlen(_onvif_snapshot_url) > 0)
    {
        strncpy(out_url, _onvif_snapshot_url, max_len - 1);
        out_url[max_len - 1] = '\0';
        return;
    }

    uint8_t ch = _profile.channel > 0 ? _profile.channel : 1;
    uint16_t configured_port = _profile.http_port > 0 ? _profile.http_port : 80;
    uint16_t https_port = configured_port == 80 ? 443 : configured_port;

    switch (_profile.vendor)
    {
        case CAM_VENDOR_HIKVISION:
            snprintf(out_url, max_len, "https://%s:%u/ISAPI/Streaming/channels/%u01/picture",
                     _profile.ip, https_port, ch);
            break;

        case CAM_VENDOR_KBVISION:
            snprintf(out_url, max_len, "https://%s:%u/cgi-bin/snapshot.cgi?channel=%u",
                     _profile.ip, https_port, ch);
            break;

        case CAM_VENDOR_EZVIZ:
        case CAM_VENDOR_YOOSEE:
        case CAM_VENDOR_GENERIC_ONVIF:
        default:
            snprintf(out_url, max_len, "https://%s:%u/onvif/snapshot",
                     _profile.ip, https_port);
            break;
    }
}

bool NetworkCameraService::onvifProbeCapabilities(char *out_service_url, size_t max_len)
{
    if (!_configured || !wifi_manager_is_connected()) return false;

    const NetworkCameraProfile request_profile = getActiveProfile();
    uint16_t configured_port = request_profile.onvif_port > 0 ? request_profile.onvif_port : 80;
    uint16_t https_port = configured_port == 80 ? 443 : configured_port;
    char probe_url[128];
    snprintf(probe_url, sizeof(probe_url), "https://%s:%u/onvif/device_service", request_profile.ip, https_port);

    HTTPClient http;
    WiFiClient plain_client;
    WiFiClientSecure secure_client;
    secure_client.setInsecure();
    if (!http.begin(secure_client, probe_url)) return false;
    http.setTimeout(1500);
    http.addHeader("Content-Type", "application/soap+xml; charset=utf-8");
    if (strlen(request_profile.username) > 0)
    {
        http.setAuthorization(request_profile.username, request_profile.password);
    }

    const char *soap_req =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Body><tds:GetCapabilities xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"/></s:Body>"
        "</s:Envelope>";

    int httpCode = http.POST(soap_req);
    if (httpCode <= 0)
    {
        http.end();
        snprintf(probe_url, sizeof(probe_url), "http://%s:%u/onvif/device_service",
                 request_profile.ip, configured_port);
        setTransportSecurity(CAM_TRANSPORT_HTTP_PLAINTEXT);
        if (http.begin(plain_client, probe_url))
        {
            http.setTimeout(1500);
            http.addHeader("Content-Type", "application/soap+xml; charset=utf-8");
            if (strlen(request_profile.username) > 0)
            {
                http.setAuthorization(request_profile.username, request_profile.password);
            }
            httpCode = http.POST(soap_req);
        }
    }
    else
    {
        setTransportSecurity(CAM_TRANSPORT_HTTPS_UNVERIFIED);
    }
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

int NetworkCameraService::fetchHttpSnapshot(uint8_t *&out_buf, size_t &current_cap)
{
    const NetworkCameraProfile request_profile = getActiveProfile();
    return fetchHttpSnapshotForProfile(out_buf, current_cap, request_profile);
}

int NetworkCameraService::fetchHttpSnapshotForProfile(uint8_t *&out_buf, size_t &current_cap,
                                                       const NetworkCameraProfile &request_profile)
{
    if (!_configured || !out_buf || current_cap == 0) return -1;

    char url[256];
    char fallback_url[256] = {0};
    if (request_profile.custom_url[0] != '\0')
    {
        strncpy(url, request_profile.custom_url, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    }
    else
    {
        uint8_t channel = request_profile.channel > 0 ? request_profile.channel : 1;
        uint16_t configured_port = request_profile.http_port > 0 ? request_profile.http_port : 80;
        uint16_t https_port = configured_port == 80 ? 443 : configured_port;
        switch (request_profile.vendor)
        {
            case CAM_VENDOR_HIKVISION:
                snprintf(url, sizeof(url), "https://%s:%u/ISAPI/Streaming/channels/%u01/picture",
                         request_profile.ip, https_port, channel);
                snprintf(fallback_url, sizeof(fallback_url), "http://%s:%u/ISAPI/Streaming/channels/%u01/picture",
                         request_profile.ip, configured_port, channel);
                break;
            case CAM_VENDOR_KBVISION:
                snprintf(url, sizeof(url), "https://%s:%u/cgi-bin/snapshot.cgi?channel=%u",
                         request_profile.ip, https_port, channel);
                snprintf(fallback_url, sizeof(fallback_url), "http://%s:%u/cgi-bin/snapshot.cgi?channel=%u",
                         request_profile.ip, configured_port, channel);
                break;
            default:
                snprintf(url, sizeof(url), "https://%s:%u/onvif/snapshot", request_profile.ip, https_port);
                snprintf(fallback_url, sizeof(fallback_url), "http://%s:%u/onvif/snapshot",
                         request_profile.ip, configured_port);
                break;
        }
    }

    HTTPClient http;
    WiFiClient plain_client;
    WiFiClientSecure secure_client;
    bool using_https = strncmp(url, "https://", 8) == 0;
    bool using_http = strncmp(url, "http://", 7) == 0;
    bool began = false;
    if (using_https)
    {
        secure_client.setInsecure();
        began = http.begin(secure_client, url);
        setTransportSecurity(CAM_TRANSPORT_HTTPS_UNVERIFIED);
    }
    else if (using_http)
    {
        began = http.begin(plain_client, url);
        setTransportSecurity(CAM_TRANSPORT_HTTP_PLAINTEXT);
    }
    if (!began)
    {
        Serial.println("[NET_CAM] ❌ Không thể khởi tạo HTTP client cho snapshot");
        return -1;
    }
    const uint32_t request_timeout_ms = 1500;
    http.setTimeout(request_timeout_ms);
    if (strlen(request_profile.username) > 0)
    {
        http.setAuthorization(request_profile.username, request_profile.password);
    }

    int httpCode = http.GET();
    if (using_https && httpCode <= 0 && fallback_url[0] != '\0')
    {
        http.end();
        setTransportSecurity(CAM_TRANSPORT_HTTP_PLAINTEXT);
        if (!http.begin(plain_client, fallback_url)) return -1;
        http.setTimeout(request_timeout_ms);
        if (strlen(request_profile.username) > 0)
        {
            http.setAuthorization(request_profile.username, request_profile.password);
        }
        httpCode = http.GET();
    }
    int bytesRead = -1;

    if (httpCode == HTTP_CODE_OK)
    {
        int expected_len = http.getSize();
        if (expected_len > (int)NET_CAM_MAX_SAFETY_LIMIT)
        {
            Serial.printf("[NET_CAM] ❌ Content-Length quá lớn (%d > 512KB limit) -> Hủy an toàn!\n", expected_len);
            http.end();
            return -1;
        }

        if (expected_len > (int)current_cap)
        {
            size_t new_cap = (expected_len + 4095) & ~4095;
            if (new_cap > NET_CAM_MAX_SAFETY_LIMIT) new_cap = NET_CAM_MAX_SAFETY_LIMIT;
            uint8_t *new_ptr = (uint8_t *)heap_caps_realloc(out_buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!new_ptr) new_ptr = (uint8_t *)realloc(out_buf, new_cap);
            if (new_ptr)
            {
                out_buf = new_ptr;
                current_cap = new_cap;
                Serial.printf("[NET_CAM] 📈 Mở rộng buffer snapshot lên %u KB\n", (unsigned int)(new_cap / 1024));
            }
            else
            {
                Serial.println("[NET_CAM] ❌ Thiếu RAM khi mở rộng snapshot buffer!");
                http.end();
                return -1;
            }
        }

        WiFiClient *stream = http.getStreamPtr();
        if (stream)
        {
            size_t total = 0;
            uint32_t start_ms = millis();
            bool reached_eoi = false;

            while (_running && (http.connected() || stream->available()) &&
                   (millis() - start_ms < request_timeout_ms))
            {
                size_t avail = stream->available();
                if (avail > 0)
                {
                    size_t to_read = avail;
                    if (total + to_read > current_cap)
                    {
                        if (current_cap < NET_CAM_MAX_SAFETY_LIMIT)
                        {
                            size_t new_cap = current_cap * 2;
                            if (new_cap > NET_CAM_MAX_SAFETY_LIMIT) new_cap = NET_CAM_MAX_SAFETY_LIMIT;
                            uint8_t *new_ptr = (uint8_t *)heap_caps_realloc(out_buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                            if (!new_ptr) new_ptr = (uint8_t *)realloc(out_buf, new_cap);
                            if (new_ptr)
                            {
                                out_buf = new_ptr;
                                current_cap = new_cap;
                            }
                        }
                        if (total + to_read > current_cap)
                        {
                            to_read = current_cap - total;
                        }
                    }
                    if (to_read == 0)
                    {
                        // Buffer đạt 512KB nhưng chưa hết frame -> dừng để tránh overflow
                        break;
                    }

                    int r = stream->readBytes(out_buf + total, to_read);
                    if (r > 0)
                    {
                        total += r;
                        // Kiểm tra nếu đã nhận đủ marker EOI kết thúc ảnh JPEG (0xFF, 0xD9)
                        if (total >= 4 && out_buf[total - 2] == 0xFF && out_buf[total - 1] == 0xD9)
                        {
                            reached_eoi = true;
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

            // Validation: Chỉ chấp nhận khung hình nếu có đủ SOI (0xFF, 0xD8) và EOI (0xFF, 0xD9)
            if (total >= 4 &&
                out_buf[0] == 0xFF && out_buf[1] == 0xD8 &&
                out_buf[total - 2] == 0xFF && out_buf[total - 1] == 0xD9)
            {
                bytesRead = (int)total;
            }
            else
            {
                Serial.printf("[NET_CAM] ❌ Frame JPEG không hợp lệ hoặc bị cắt bớt (%u bytes, EOI: %s) -> Hủy bỏ\n",
                              (unsigned int)total, reached_eoi ? "YES" : "NO");
                bytesRead = -1;
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


