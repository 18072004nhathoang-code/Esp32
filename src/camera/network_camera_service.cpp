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

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef CAMERA_TLS_CA_CERT
#define CAMERA_TLS_CA_CERT ""
#endif

NetworkCameraService g_network_camera;

#define NET_CAM_DEFAULT_BUF_CAP (256 * 1024) // 256 KB trong PSRAM cho Snapshot JPEG
#define NET_CAM_MAX_SAFETY_LIMIT (512 * 1024) // Giới hạn an toàn tối đa 512 KB

#include <utility>

namespace
{
class BoundedBufferStream final : public Stream
{
public:
    BoundedBufferStream(uint8_t *&buffer, size_t &capacity, size_t limit,
                        volatile bool *running)
        : _buffer(buffer), _capacity(capacity), _limit(limit), _running(running) {}

    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t *data, size_t len) override
    {
        if (!data || len == 0) return 0;
        if ((_running && !*_running) || len > _limit - _size)
        {
            _failed = true;
            return 0;
        }
        const size_t required = _size + len;
        if (required > _capacity)
        {
            size_t new_capacity = _capacity ? _capacity : 4096;
            while (new_capacity < required && new_capacity < _limit)
                new_capacity = new_capacity > _limit / 2 ? _limit : new_capacity * 2;
            uint8_t *grown = static_cast<uint8_t *>(
                heap_caps_realloc(_buffer, new_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!grown)
            {
                _failed = true;
                return 0;
            }
            _buffer = grown;
            _capacity = new_capacity;
        }
        memcpy(_buffer + _size, data, len);
        _size += len;
        return len;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    size_t size() const { return _size; }
    bool failed() const { return _failed; }

private:
    uint8_t *&_buffer;
    size_t &_capacity;
    size_t _limit;
    volatile bool *_running;
    size_t _size = 0;
    bool _failed = false;
};

bool url_has_scheme(const char *url, const char *scheme)
{
    return url && strncmp(url, scheme, strlen(scheme)) == 0;
}
}

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
    CameraTransportSecurity old = CAM_TRANSPORT_NONE;
    if (_config_mutex && xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        old = _transport_security;
        _transport_security = security;
        xSemaphoreGive(_config_mutex);
    }
    if (old == security) return;
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
    if (!_config_mutex) return CAM_STATE_ERROR;
    if (xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return CAM_STATE_ERROR;
    const CameraRuntimeState state = _runtime_state;
    xSemaphoreGive(_config_mutex);
    return state;
}

CameraTransportSecurity NetworkCameraService::getTransportSecurity() const
{
    if (!_config_mutex) return CAM_TRANSPORT_NONE;
    if (xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return CAM_TRANSPORT_NONE;
    const CameraTransportSecurity security = _transport_security;
    xSemaphoreGive(_config_mutex);
    return security;
}

bool NetworkCameraService::snapshotState(NetworkCameraProfile &profile, bool &configured,
                                         CameraRuntimeState &state) const
{
    if (!_config_mutex || xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return false;
    profile = _profile;
    configured = _configured;
    state = _runtime_state;
    xSemaphoreGive(_config_mutex);
    return true;
}

bool NetworkCameraService::saveProfileToNVS()
{
    Preferences prefs;
    if (!prefs.begin("netcam", false)) return false;

    NetworkCameraProfile prof = getActiveProfile();
    bool saved = true;
    saved = (prefs.putString("name", prof.name) == strlen(prof.name)) && saved;
    saved = (prefs.putString("ip", prof.ip) == strlen(prof.ip)) && saved;
    saved = (prefs.putUShort("http_port", prof.http_port) > 0) && saved;
    saved = (prefs.putUShort("rtsp_port", prof.rtsp_port) > 0) && saved;
    saved = (prefs.putUShort("onvif_port", prof.onvif_port) > 0) && saved;
    saved = (prefs.putUChar("vendor", (uint8_t)prof.vendor) > 0) && saved;
    saved = (prefs.putUChar("proto", (uint8_t)prof.protocol) > 0) && saved;
    saved = (prefs.putUChar("ch", prof.channel) > 0) && saved;
    saved = (prefs.putUChar("security", (uint8_t)prof.security_mode) > 0) && saved;
    saved = (prefs.putString("user", prof.username) == strlen(prof.username)) && saved;
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
    prof.security_mode = (CameraSecurityMode)prefs.getUChar("security", (uint8_t)CAM_SECURITY_TLS_VERIFIED);
    if (prof.security_mode < CAM_SECURITY_TLS_VERIFIED || prof.security_mode > CAM_SECURITY_HTTP_PLAINTEXT)
        prof.security_mode = CAM_SECURITY_TLS_VERIFIED;
    String user = prefs.getString("user", "");
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

    Serial.printf("[NET_CAM] Đã nạp profile camera '%s' tại %s (password không lưu)\n",
                  prof.name, prof.ip);
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

    if (_config_mutex && xSemaphoreTake(_config_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (_runtime_state == CAM_STATE_STARTING) _runtime_state = CAM_STATE_RUNNING;
        xSemaphoreGive(_config_mutex);
    }

    while (_running)
    {
        NetworkCameraProfile cur_prof;
        bool is_cfg = false;
        CameraRuntimeState cur_state = CAM_STATE_NOT_CONFIGURED;

        if (!snapshotState(cur_prof, is_cfg, cur_state))
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Không tự động thực hiện request nếu thiếu password
        if (cur_state == CAM_STATE_PASSWORD_REQUIRED || (strlen(cur_prof.username) > 0 && strlen(cur_prof.password) == 0))
        {
            if (_config_mutex && xSemaphoreTake(_config_mutex, portMAX_DELAY) == pdTRUE)
            {
                _connected = false;
                _runtime_state = CAM_STATE_PASSWORD_REQUIRED;
                xSemaphoreGive(_config_mutex);
            }
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
                    if (!has_dim) bytes = -1;

                    // Hoán đổi atomic back buffer sang front buffer kèm dung lượng thật
                    if (bytes > 0 && _frame_mutex && xSemaphoreTake(_frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
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

                            if (_config_mutex && xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                            {
                                _connected = true;
                                xSemaphoreGive(_config_mutex);
                            }
                            _snapshot_status = CAM_STATUS_READY;
                        }
                        xSemaphoreGive(_frame_mutex);
                    }
                }
                else
                {
                    if (_config_mutex && xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                    {
                        _connected = false;
                        xSemaphoreGive(_config_mutex);
                    }
                    if (_snapshot_status != CAM_STATUS_READY)
                    {
                        _snapshot_status = CAM_STATUS_ERROR;
                    }
                }
            }
        }
        else
        {
            if (_config_mutex && xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                _connected = false;
                if (!is_cfg) _runtime_state = CAM_STATE_NOT_CONFIGURED;
                xSemaphoreGive(_config_mutex);
            }
        }

        // Delay 1500ms nhưng kiểm tra _running mỗi 50ms để dừng tức thì khi stop() được gọi
        for (int i = 0; i < 30 && _running; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (_config_mutex && xSemaphoreTake(_config_mutex, portMAX_DELAY) == pdTRUE)
    {
        _worker_task_handle = nullptr;
        _connected = false;
        if (_runtime_state == CAM_STATE_STOPPING || _runtime_state == CAM_STATE_RUNNING)
            _runtime_state = CAM_STATE_STOPPED;
        xSemaphoreGive(_config_mutex);
    }
    if (_worker_exit_sem)
    {
        xSemaphoreGive(_worker_exit_sem);
    }
    vTaskDelete(NULL);
}

bool NetworkCameraService::start()
{
    if (!ensureSynchronizationPrimitives()) return false;
    if (xSemaphoreTake(_config_mutex, portMAX_DELAY) != pdTRUE) return false;
    const NetworkCameraProfile request_profile = _profile;
    if (!_configured)
    {
        _runtime_state = CAM_STATE_NOT_CONFIGURED;
        xSemaphoreGive(_config_mutex);
        return false;
    }

    if (strlen(request_profile.username) > 0 && strlen(request_profile.password) == 0)
    {
        Serial.println("[NET_CAM] ⚠️ Cần nhập mật khẩu camera để xác thực!");
        _runtime_state = CAM_STATE_PASSWORD_REQUIRED;
        xSemaphoreGive(_config_mutex);
        return false;
    }

    if (request_profile.protocol == CAM_PROTO_HTTP_SNAPSHOT)
    {
        if (_runtime_state == CAM_STATE_STOPPING || _worker_task_handle != nullptr)
        {
            const bool already_running = _runtime_state == CAM_STATE_RUNNING || _runtime_state == CAM_STATE_STARTING;
            xSemaphoreGive(_config_mutex);
            return already_running;
        }
        if (_worker_exit_sem == nullptr)
        {
            _worker_exit_sem = xSemaphoreCreateBinary();
        }
        if (!_worker_exit_sem)
        {
            _runtime_state = CAM_STATE_ERROR;
            xSemaphoreGive(_config_mutex);
            Serial.println("[NET_CAM] ❌ Chế độ suy giảm: không tạo được semaphore worker");
            return false;
        }
        xSemaphoreTake(_worker_exit_sem, 0); // Xóa token cũ nếu có

        _running = true;
        _runtime_state = CAM_STATE_STARTING;
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
            _worker_task_handle = nullptr;
            xSemaphoreGive(_config_mutex);
            Serial.println("[NET_CAM] ❌ Không thể tạo NetCamWorker task!");
            return false;
        }
        xSemaphoreGive(_config_mutex);
        Serial.println("[NET_CAM] Snapshot worker started");
        return true;
    }
    else if (request_profile.protocol == CAM_PROTO_MJPEG)
    {
        _connected = false;
        _runtime_state = CAM_STATE_ERROR;
        _mjpeg_status = CAM_STATUS_NOT_IMPLEMENTED;
        xSemaphoreGive(_config_mutex);
        Serial.println("[NET_CAM] ⚠️ Giao thức MJPEG chưa hoàn chỉnh -> Đánh dấu NOT_IMPLEMENTED");
        return false;
    }
    else if (request_profile.protocol == CAM_PROTO_RTSP)
    {
        _connected = false;
        _runtime_state = CAM_STATE_ERROR;
        _rtsp_status = CAM_STATUS_NOT_IMPLEMENTED;
        xSemaphoreGive(_config_mutex);
        Serial.println("[NET_CAM] ⚠️ Giao thức RTSP/H.264 chưa hoàn chỉnh -> Đánh dấu NOT_IMPLEMENTED");
        return false;
    }

    xSemaphoreGive(_config_mutex);
    return false;
}

bool NetworkCameraService::stop(uint32_t timeout_ms)
{
    if (!ensureSynchronizationPrimitives()) return false;
    if (xSemaphoreTake(_config_mutex, portMAX_DELAY) != pdTRUE) return false;
    if (_worker_task_handle == nullptr)
    {
        _running = false;
        _connected = false;
        if (_runtime_state != CAM_STATE_NOT_CONFIGURED && _runtime_state != CAM_STATE_PASSWORD_REQUIRED)
            _runtime_state = CAM_STATE_STOPPED;
        xSemaphoreGive(_config_mutex);
        return true;
    }
    _runtime_state = CAM_STATE_STOPPING;
    _running = false;
    xSemaphoreGive(_config_mutex);
    if (_worker_exit_sem)
    {
        xSemaphoreTake(_worker_exit_sem, pdMS_TO_TICKS(timeout_ms));
    }
    if (xSemaphoreTake(_config_mutex, portMAX_DELAY) != pdTRUE) return false;
    const bool exited = _worker_task_handle == nullptr;
    if (!exited)
    {
        _runtime_state = CAM_STATE_STOPPING;
        xSemaphoreGive(_config_mutex);
        Serial.println("[NET_CAM] Worker stop timeout; tài nguyên vẫn được giữ nguyên.");
        return false;
    }
    _connected = false;
    if (_runtime_state != CAM_STATE_PASSWORD_REQUIRED)
    {
        _runtime_state = CAM_STATE_STOPPED;
    }
    xSemaphoreGive(_config_mutex);
    Serial.println("[NET_CAM] ⏹ Đã dừng dịch vụ IP Camera.");
    return true;
}

bool NetworkCameraService::isConnected() const
{
    if (!_config_mutex || xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    const bool connected = _connected;
    xSemaphoreGive(_config_mutex);
    return connected;
}

NetworkCameraProfile NetworkCameraService::getActiveProfile() const
{
    NetworkCameraProfile prof;
    memset(&prof, 0, sizeof(prof));
    if (_config_mutex && xSemaphoreTake(_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        prof = _profile;
        xSemaphoreGive(_config_mutex);
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
    if (_frame_mutex && xSemaphoreTake(_frame_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        if (_frame_front.buf == nullptr || _frame_front.len == 0)
        {
            xSemaphoreGive(_frame_mutex);
            return nullptr;
        }
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
    const NetworkCameraProfile profile = getActiveProfile();

    if (strlen(profile.custom_url) > 0)
    {
        if (mask_credential)
        {
            sanitizeUrl(profile.custom_url, out_url, max_len);
        }
        else
        {
            strncpy(out_url, profile.custom_url, max_len - 1);
            out_url[max_len - 1] = '\0';
        }
        return;
    }

    uint8_t ch = profile.channel > 0 ? profile.channel : 1;
    uint16_t port = profile.rtsp_port > 0 ? profile.rtsp_port : 554;

    char encoded_user[48] = {0};
    char encoded_pass[48] = {0};
    if (mask_credential)
    {
        strcpy(encoded_user, "***");
        strcpy(encoded_pass, "***");
    }
    else
    {
        urlEncode(profile.username, encoded_user, sizeof(encoded_user));
        urlEncode(profile.password, encoded_pass, sizeof(encoded_pass));
    }

    switch (profile.vendor)
    {
        case CAM_VENDOR_HIKVISION:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/Streaming/Channels/%u01",
                     encoded_user, encoded_pass, profile.ip, port, ch);
            break;

        case CAM_VENDOR_KBVISION:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/cam/realmonitor?channel=%u&subtype=0",
                     encoded_user, encoded_pass, profile.ip, port, ch);
            break;

        case CAM_VENDOR_EZVIZ:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/h264/ch%u/main/av_stream",
                     encoded_user, encoded_pass, profile.ip, port, ch);
            break;

        case CAM_VENDOR_YOOSEE:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/onvif1",
                     encoded_user, encoded_pass, profile.ip, port);
            break;

        case CAM_VENDOR_GENERIC_ONVIF:
        default:
            snprintf(out_url, max_len, "rtsp://%s:%s@%s:%u/live/ch%u",
                     encoded_user, encoded_pass, profile.ip, port, ch);
            break;
    }
}

void NetworkCameraService::buildSnapshotUrl(char *out_url, size_t max_len) const
{
    if (!out_url || max_len == 0) return;
    const NetworkCameraProfile profile = getActiveProfile();

    if (profile.custom_url[0] != '\0')
    {
        strncpy(out_url, profile.custom_url, max_len - 1);
        out_url[max_len - 1] = '\0';
        return;
    }

    if (strlen(_onvif_snapshot_url) > 0)
    {
        strncpy(out_url, _onvif_snapshot_url, max_len - 1);
        out_url[max_len - 1] = '\0';
        return;
    }

    uint8_t ch = profile.channel > 0 ? profile.channel : 1;
    uint16_t configured_port = profile.http_port > 0 ? profile.http_port : 80;
    uint16_t https_port = configured_port == 80 ? 443 : configured_port;
    const bool plaintext = profile.security_mode == CAM_SECURITY_HTTP_PLAINTEXT;
    const char *scheme = plaintext ? "http" : "https";
    const uint16_t port = plaintext ? configured_port : https_port;

    switch (profile.vendor)
    {
        case CAM_VENDOR_HIKVISION:
            snprintf(out_url, max_len, "%s://%s:%u/ISAPI/Streaming/channels/%u01/picture",
                     scheme, profile.ip, port, ch);
            break;

        case CAM_VENDOR_KBVISION:
            snprintf(out_url, max_len, "%s://%s:%u/cgi-bin/snapshot.cgi?channel=%u",
                     scheme, profile.ip, port, ch);
            break;

        case CAM_VENDOR_EZVIZ:
        case CAM_VENDOR_YOOSEE:
        case CAM_VENDOR_GENERIC_ONVIF:
        default:
            snprintf(out_url, max_len, "%s://%s:%u/onvif/snapshot",
                     scheme, profile.ip, port);
            break;
    }
}

bool NetworkCameraService::onvifProbeCapabilities(char *out_service_url, size_t max_len)
{
    const NetworkCameraProfile request_profile = getActiveProfile();
    if ((!request_profile.ip[0] && !request_profile.custom_url[0]) || !wifi_manager_is_connected()) return false;
    uint16_t configured_port = request_profile.onvif_port > 0 ? request_profile.onvif_port : 80;
    uint16_t https_port = configured_port == 80 ? 443 : configured_port;
    char probe_url[128];
    const bool plaintext = request_profile.security_mode == CAM_SECURITY_HTTP_PLAINTEXT;
    snprintf(probe_url, sizeof(probe_url), "%s://%s:%u/onvif/device_service",
             plaintext ? "http" : "https", request_profile.ip,
             plaintext ? configured_port : https_port);

    HTTPClient http;
    WiFiClient plain_client;
    WiFiClientSecure secure_client;
    bool began = false;
    if (plaintext)
    {
        setTransportSecurity(CAM_TRANSPORT_HTTP_PLAINTEXT);
        began = http.begin(plain_client, probe_url);
    }
    else if (request_profile.security_mode == CAM_SECURITY_TLS_INSECURE)
    {
        secure_client.setInsecure();
        setTransportSecurity(CAM_TRANSPORT_HTTPS_UNVERIFIED);
        began = http.begin(secure_client, probe_url);
    }
    else if (CAMERA_TLS_CA_CERT[0] != '\0')
    {
        secure_client.setCACert(CAMERA_TLS_CA_CERT);
        setTransportSecurity(CAM_TRANSPORT_HTTPS_VERIFIED);
        began = http.begin(secure_client, probe_url);
    }
    if (!began) return false;
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
    if (!out_buf || current_cap == 0 || !_running) return -1;

    char url[256] = {};
    if (request_profile.custom_url[0] != '\0')
    {
        strncpy(url, request_profile.custom_url, sizeof(url) - 1);
    }
    else
    {
        uint8_t channel = request_profile.channel > 0 ? request_profile.channel : 1;
        uint16_t configured_port = request_profile.http_port > 0 ? request_profile.http_port : 80;
        uint16_t https_port = configured_port == 80 ? 443 : configured_port;
        const bool plaintext = request_profile.security_mode == CAM_SECURITY_HTTP_PLAINTEXT;
        const char *scheme = plaintext ? "http" : "https";
        const uint16_t port = plaintext ? configured_port : https_port;
        switch (request_profile.vendor)
        {
            case CAM_VENDOR_HIKVISION:
                snprintf(url, sizeof(url), "%s://%s:%u/ISAPI/Streaming/channels/%u01/picture",
                         scheme, request_profile.ip, port, channel);
                break;
            case CAM_VENDOR_KBVISION:
                snprintf(url, sizeof(url), "%s://%s:%u/cgi-bin/snapshot.cgi?channel=%u",
                         scheme, request_profile.ip, port, channel);
                break;
            default:
                snprintf(url, sizeof(url), "%s://%s:%u/onvif/snapshot",
                         scheme, request_profile.ip, port);
                break;
        }
    }

    const bool using_https = url_has_scheme(url, "https://");
    const bool using_http = url_has_scheme(url, "http://");
    if ((!using_https && !using_http) ||
        (request_profile.security_mode == CAM_SECURITY_HTTP_PLAINTEXT && !using_http) ||
        (request_profile.security_mode != CAM_SECURITY_HTTP_PLAINTEXT && !using_https))
    {
        Serial.println("[NET_CAM] URL scheme rejected by selected security mode");
        return -1;
    }

    HTTPClient http;
    WiFiClient plain_client;
    WiFiClientSecure secure_client;
    bool began = false;
    if (using_https)
    {
        if (request_profile.security_mode == CAM_SECURITY_TLS_VERIFIED)
        {
            if (CAMERA_TLS_CA_CERT[0] == '\0')
            {
                Serial.println("[NET_CAM] Verified TLS requires CAMERA_TLS_CA_CERT");
                return -1;
            }
            secure_client.setCACert(CAMERA_TLS_CA_CERT);
            setTransportSecurity(CAM_TRANSPORT_HTTPS_VERIFIED);
        }
        else
        {
            secure_client.setInsecure();
            setTransportSecurity(CAM_TRANSPORT_HTTPS_UNVERIFIED);
        }
        began = http.begin(secure_client, url);
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

    const int httpCode = http.GET();
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

        if (_running)
        {
            BoundedBufferStream sink(out_buf, current_cap, NET_CAM_MAX_SAFETY_LIMIT, &_running);
            const int written = http.writeToStream(&sink);
            const size_t total = sink.size();
            const bool complete_length = expected_len <= 0 || total == static_cast<size_t>(expected_len);
            if (written >= 0 && !sink.failed() && _running && complete_length && total >= 4 &&
                out_buf[0] == 0xFF && out_buf[1] == 0xD8 &&
                out_buf[total - 2] == 0xFF && out_buf[total - 1] == 0xD9)
            {
                bytesRead = (int)total;
            }
            else
            {
                Serial.printf("[NET_CAM] Snapshot rejected: incomplete/invalid JPEG (%u bytes)\n",
                              (unsigned int)total);
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


