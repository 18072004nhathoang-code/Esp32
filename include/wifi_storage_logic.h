/**
 * @file wifi_storage_logic.h
 * @brief Logic nghiệp vụ cho việc lưu trữ, xác thực và phục hồi thông tin WiFi an toàn (Atomic/Transactional NVS)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// FNV-1a 32-bit checksum cho cặp SSID và Mật khẩu
inline uint32_t wifi_credentials_checksum(const char *ssid, const char *pass)
{
    uint32_t hash = 2166136261u;
    if (ssid)
    {
        while (*ssid)
        {
            hash ^= static_cast<uint8_t>(*ssid++);
            hash *= 16777619u;
        }
    }
    hash ^= 0xFFu;
    hash *= 16777619u;
    if (pass)
    {
        while (*pass)
        {
            hash ^= static_cast<uint8_t>(*pass++);
            hash *= 16777619u;
        }
    }
    if (hash == 0) hash = 1;
    return hash;
}

// Kiểm tra tính toàn vẹn của thông tin xác thực
inline bool wifi_credentials_verify(const char *ssid, const char *pass, uint32_t checksum)
{
    if (!ssid || !*ssid || checksum == 0) return false;
    return checksum == wifi_credentials_checksum(ssid, pass);
}

// Xác định xem trạng thái kết nối có đủ điều kiện để ghi NVS hay không:
// Phải kết nối thành công VÀ có IP hợp lệ (khác 0.0.0.0) VÀ đúng thế hệ yêu cầu.
inline bool wifi_can_commit_save(bool should_save, bool is_connected, bool has_valid_ip,
                                 uint32_t active_gen, uint32_t pending_save_gen,
                                 bool manual_disconnect)
{
    return should_save && is_connected && has_valid_ip &&
           active_gen != 0 && active_gen == pending_save_gen && !manual_disconnect;
}

// Xác định xem yêu cầu lưu có được bảo toàn qua các lần retry kết nối lại không
inline bool wifi_retry_preserves_save(bool should_save, bool manual_disconnect, bool forgetting)
{
    return should_save && !manual_disconnect && !forgetting;
}

// Xác định xem có cần phục hồi mạng tốt từ NVS khi kết nối mạng mới bị thất bại hay không
inline bool wifi_failed_attempt_should_restore_saved(bool was_attempting_new_save,
                                                    bool has_saved_network)
{
    return was_attempting_new_save && has_saved_network;
}

// Phát hiện các SSID mẫu/dummy không được tự động kết nối
inline bool wifi_is_sample_ssid(const char *ssid)
{
    if (!ssid || !*ssid) return true;
    if (strcmp(ssid, "YourSSID") == 0 || strcmp(ssid, "MySSID") == 0 ||
        strcmp(ssid, "MyHomeWiFi") == 0 || strcmp(ssid, "SSID") == 0 ||
        strcmp(ssid, "example") == 0 || strcmp(ssid, "default") == 0)
        return true;
    return false;
}
