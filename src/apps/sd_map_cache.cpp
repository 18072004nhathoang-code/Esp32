/**
 * @file sd_map_cache.cpp
 * @brief Triển khai phân hệ bộ nhớ đệm bản đồ trên thẻ MicroSD FAT32 cho ESP32-S3
 * Dùng storage_manager cho thẻ SDMMC của ES3C28P.
 */

#include "sd_map_cache.h"
#include "../display/lvgl_port.h"
#include "../storage/storage_manager.h"
#include "firmware_contracts.h"

static bool s_cache_ready = false;

static bool sd_acquire_bus(uint32_t timeout_ms = 1000)
{
    return storage_lock(timeout_ms);
}

static void sd_release_bus(void)
{
    storage_unlock();
}

bool sd_map_cache_init(void)
{
    s_cache_ready = false;
    if (!storage_is_available())
    {
        Serial.println("[SD_CACHE] Đang khởi tạo thẻ nhớ MicroSD FAT32...");
        if (!storage_init())
        {
            Serial.println("[SD_CACHE] ⚠️ Không tìm thấy thẻ nhớ MicroSD hoặc khởi tạo thất bại!");
            return false;
        }
    }

    if (!sd_acquire_bus()) return false;

    // Kiểm tra và tạo thư mục /maps nếu chưa có
    bool ready = storage_get_fs().exists(SD_MAPS_DIR);
    if (!ready)
    {
        Serial.printf("[SD_CACHE] Tạo thư mục lưu trữ bản đồ: %s\n", SD_MAPS_DIR);
        ready = storage_get_fs().mkdir(SD_MAPS_DIR);
    }

    sd_release_bus();
    s_cache_ready = ready;
    if (!ready) Serial.println("[SD_CACHE] ⚠️ Không thể tạo/xác minh thư mục /maps!");
    return ready;
}

bool sd_map_cache_is_available(void)
{
    return storage_is_available() && s_cache_ready;
}

void sd_map_cache_get_filename(char *out_path, size_t max_len, double lat, double lon, int zoom, const char *maptype)
{
    if (!out_path || max_len == 0) return;
    const char *type = (maptype && strlen(maptype) > 0) ? maptype : "roadmap";
    snprintf(out_path, max_len, "%s/%.4f_%.4f_z%d_%s.jpg", SD_MAPS_DIR, lat, lon, zoom, type);
}

bool sd_map_cache_exists(double lat, double lon, int zoom, const char *maptype)
{
    if (!sd_map_cache_is_available()) return false;
    if (!sd_acquire_bus(200)) return false;

    char filepath[128];
    sd_map_cache_get_filename(filepath, sizeof(filepath), lat, lon, zoom, maptype);

    bool exists = storage_get_fs().exists(filepath);
    sd_release_bus();

    if (exists)
    {
        Serial.printf("[SD_CACHE] 🎯 CACHE HIT: Đã tìm thấy tệp %s\n", filepath);
    }
    else
    {
        Serial.printf("[SD_CACHE] 💨 CACHE MISS: Chưa có tệp %s\n", filepath);
    }
    return exists;
}

int sd_map_cache_read(double lat, double lon, int zoom, const char *maptype, uint8_t *out_buf, size_t max_size)
{
    if (!sd_map_cache_is_available() || !out_buf || max_size == 0) return -1;
    if (!sd_acquire_bus(500)) return -1;

    char filepath[128];
    sd_map_cache_get_filename(filepath, sizeof(filepath), lat, lon, zoom, maptype);

    File file = storage_get_fs().open(filepath, FILE_READ);
    if (!file)
    {
        Serial.printf("[SD_CACHE] ❌ Không thể mở tệp đọc: %s\n", filepath);
        sd_release_bus();
        return -1;
    }

    size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > max_size)
    {
        Serial.printf("[SD_CACHE] ❌ Dung lượng tệp không hợp lệ: %u bytes (Max: %u)\n", (unsigned int)fileSize, (unsigned int)max_size);
        file.close();
        sd_release_bus();
        return -1;
    }

    size_t bytesRead = file.read(out_buf, fileSize);
    file.close();
    sd_release_bus();

    if (bytesRead != fileSize)
    {
        Serial.printf("[SD_CACHE] ❌ Đọc thiếu: %u/%u bytes\n", (unsigned)bytesRead, (unsigned)fileSize);
        return -1;
    }
    Serial.printf("[SD_CACHE] 📖 Đọc thành công %u bytes từ %s\n", (unsigned int)bytesRead, filepath);
    return (int)bytesRead;
}

bool sd_map_cache_write(double lat, double lon, int zoom, const char *maptype, const uint8_t *in_buf, size_t size)
{
    if (!sd_map_cache_is_available() || !in_buf || size == 0) return false;
    if (!sd_acquire_bus(500)) return false;

    char filepath[128];
    sd_map_cache_get_filename(filepath, sizeof(filepath), lat, lon, zoom, maptype);
    char temp_path[136];
    char backup_path[136];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", filepath);
    snprintf(backup_path, sizeof(backup_path), "%s.bak", filepath);
    fs::FS &fs = storage_get_fs();

    if (fs.exists(temp_path)) fs.remove(temp_path);
    File file = fs.open(temp_path, FILE_WRITE);
    if (!file)
    {
        Serial.printf("[SD_CACHE] ❌ Không thể mở tệp ghi: %s\n", filepath);
        sd_release_bus();
        return false;
    }

    size_t bytesWritten = file.write(in_buf, size);
    file.flush();
    file.close();
    bool temp_valid = bytesWritten == size;
    if (temp_valid)
    {
        File verify = fs.open(temp_path, FILE_READ);
        temp_valid = verify && verify.size() == size;
        if (verify) verify.close();
    }

    bool committed = false;
    if (temp_valid)
    {
        if (fs.exists(backup_path)) fs.remove(backup_path);
        const bool had_old = fs.exists(filepath);
        const bool backed_up = !had_old || fs.rename(filepath, backup_path);
        if (cache_temp_can_replace(bytesWritten == size, temp_valid, backed_up))
        {
            committed = fs.rename(temp_path, filepath);
            if (committed)
            {
                if (had_old) fs.remove(backup_path);
            }
            else if (had_old)
            {
                fs.rename(backup_path, filepath);
            }
        }
    }
    if (fs.exists(temp_path)) fs.remove(temp_path);
    sd_release_bus();

    if (committed)
        Serial.printf("[SD_CACHE] 💾 Đã commit cache: %s (%u bytes)\n", filepath, (unsigned)size);
    else
        Serial.printf("[SD_CACHE] ⚠️ Ghi cache thất bại; giữ bản cũ: %u/%u bytes\n",
                      (unsigned)bytesWritten, (unsigned)size);
    return committed;
}

uint64_t sd_map_cache_get_free_mb(void)
{
    return storage_get_free_mb();
}
