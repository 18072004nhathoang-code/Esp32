/**
 * @file wifi_storage_logic.h
 * @brief Logic nghiệp vụ cho việc lưu trữ, xác thực và phục hồi thông tin WiFi an toàn (Atomic/Transactional NVS 2-slot blob)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define WIFI_BLOB_MAGIC 0x57494649u // "WIFI"
#define WIFI_BLOB_VERSION 1

#pragma pack(push, 1)
struct WifiCredentialBlob
{
    uint32_t magic;      // 0x57494649 ("WIFI")
    uint16_t version;    // 1
    uint16_t reserved;   // 0
    uint32_t sequence;   // Monotonic sequence number (1, 2, 3...)
    char ssid[33];       // null-terminated SSID (max 32 chars + 1)
    char pass[65];       // null-terminated Password (max 64 chars + 1)
    uint32_t checksum;   // FNV-1a checksum over preceding fields
};
#pragma pack(pop)

// FNV-1a 32-bit checksum cho khối bộ nhớ nhị phân
inline uint32_t wifi_fnv1a_32(const void *data, size_t length)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    if (hash == 0) hash = 1;
    return hash;
}

// Checksum cho WifiCredentialBlob (tính trên toàn bộ các trường trước checksum)
inline uint32_t wifi_blob_checksum(const WifiCredentialBlob &blob)
{
    return wifi_fnv1a_32(&blob, offsetof(WifiCredentialBlob, checksum));
}

// Xác thực tính toàn vẹn của một WifiCredentialBlob
inline bool wifi_blob_verify(const WifiCredentialBlob &blob)
{
    if (blob.magic != WIFI_BLOB_MAGIC || blob.version != WIFI_BLOB_VERSION || blob.sequence == 0)
        return false;
    if (blob.ssid[sizeof(blob.ssid) - 1] != '\0' || blob.ssid[0] == '\0')
        return false;
    if (blob.pass[sizeof(blob.pass) - 1] != '\0')
        return false;
    if (blob.checksum == 0)
        return false;
    return blob.checksum == wifi_blob_checksum(blob);
}

// Lựa chọn slot active (0 hoặc 1) dựa trên tính hợp lệ và số sequence
inline int wifi_choose_active_slot(bool slot0_valid, uint32_t seq0, bool slot1_valid, uint32_t seq1)
{
    if (slot0_valid && slot1_valid)
    {
        return (static_cast<int32_t>(seq1 - seq0) > 0) ? 1 : 0;
    }
    if (slot0_valid) return 0;
    if (slot1_valid) return 1;
    return -1;
}

// Kiểm tra xem blob có khớp với SSID và mật khẩu cần so sánh không
inline bool wifi_credentials_match(const WifiCredentialBlob &blob, const char *ssid, const char *pass)
{
    if (!ssid || strcmp(blob.ssid, ssid) != 0) return false;
    const char *safe_p = pass ? pass : "";
    return strcmp(blob.pass, safe_p) == 0;
}

// FNV-1a 32-bit checksum cho cặp SSID và Mật khẩu chuỗi rời (Legacy support)
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

// Kiểm tra tính toàn vẹn của thông tin xác thực dạng chuỗi rời (Legacy support)
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
