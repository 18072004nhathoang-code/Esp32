/**
 * @file sd_map_cache.cpp
 * @brief Triển khai phân hệ bộ nhớ đệm bản đồ trên thẻ MicroSD FAT32 cho ESP32-S3
 * Dùng storage_manager cho thẻ SDMMC của ES3C28P.
 */

#include "sd_map_cache.h"
#include "../display/lvgl_port.h"
#include "../storage/storage_manager.h"
#include "service_state_logic.h"
#include "firmware_contracts.h"
#include <atomic>

static std::atomic<bool> s_cache_ready{false};

static bool sd_acquire_bus(uint32_t timeout_ms = 1000)
{
    return storage_lock(timeout_ms);
}

static void sd_release_bus(void)
{
    storage_unlock();
}

static bool valid_cached_jpeg_locked(fs::FS &fs, const char *path)
{
    File file = fs.open(path, FILE_READ);
    if (!file || file.size() < 4)
    {
        if (file) file.close();
        return false;
    }
    uint8_t first[2] = {}, last[2] = {};
    const bool ok = file.read(first, sizeof(first)) == sizeof(first) &&
                    file.seek(file.size() - 2) &&
                    file.read(last, sizeof(last)) == sizeof(last) &&
                    first[0] == 0xFF && first[1] == 0xD8 &&
                    last[0] == 0xFF && last[1] == 0xD9;
    file.close();
    return ok;
}

static String cache_entry_path(const char *name)
{
    if (!name || !*name) return String();
    String path(name);
    if (!path.startsWith("/")) path = String(SD_MAPS_DIR) + "/" + path;
    return path;
}

static void recover_cache_suffix_locked(fs::FS &fs, const char *suffix)
{
    File dir = fs.open(SD_MAPS_DIR);
    if (!dir || !dir.isDirectory())
    {
        if (dir) dir.close();
        return;
    }
    File entry = dir.openNextFile();
    while (entry)
    {
        String path = cache_entry_path(entry.name());
        entry.close();
        if (path.endsWith(suffix))
        {
            String final_path = path.substring(0, path.length() - strlen(suffix));
            const bool final_valid = fs.exists(final_path.c_str()) &&
                                     valid_cached_jpeg_locked(fs, final_path.c_str());
            if (final_valid)
            {
                fs.remove(path.c_str());
            }
            else
            {
                if (fs.exists(final_path.c_str())) fs.remove(final_path.c_str());
                if (valid_cached_jpeg_locked(fs, path.c_str()))
                {
                    if (fs.rename(path.c_str(), final_path.c_str()))
                        Serial.printf("[SD_CACHE] Recovered %s from %s\n",
                                      final_path.c_str(), suffix);
                }
                else
                {
                    fs.remove(path.c_str());
                }
            }
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

bool sd_map_cache_init(void)
{
    s_cache_ready.store(false, std::memory_order_release);
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

    if (ready)
    {
        fs::FS &fs = storage_get_fs();
        recover_cache_suffix_locked(fs, ".bak");
        recover_cache_suffix_locked(fs, ".tmp");
    }

    sd_release_bus();
    s_cache_ready.store(ready, std::memory_order_release);
    if (!ready) Serial.println("[SD_CACHE] ⚠️ Không thể tạo/xác minh thư mục /maps!");
    return ready;
}

bool sd_map_cache_is_available(void)
{
    return storage_is_available() && s_cache_ready.load(std::memory_order_acquire);
}

void sd_map_cache_get_filename(char *out_path, size_t max_len, double lat, double lon, int zoom, const char *maptype)
{
    format_map_tile_cache_path(out_path, max_len, lat, lon, zoom, maptype);
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

bool sd_map_cache_write_guarded(double lat, double lon, int zoom, const char *maptype,
                                const uint8_t *in_buf, size_t size,
                                SdMapCacheCommitGuard can_commit, void *context)
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

    const bool final_valid_before = fs.exists(filepath) &&
                                    valid_cached_jpeg_locked(fs, filepath);
    const bool recoverable_temp = fs.exists(temp_path) &&
                                  valid_cached_jpeg_locked(fs, temp_path);
    const bool recoverable_backup = fs.exists(backup_path) &&
                                    valid_cached_jpeg_locked(fs, backup_path);
    if (!final_valid_before && (recoverable_temp || recoverable_backup))
    {
        sd_release_bus();
        return false;
    }
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
        temp_valid = temp_valid && valid_cached_jpeg_locked(fs, temp_path);
    }

    bool committed = false;
    bool backup_created = false;
    bool new_file_installed = false;
    bool commit_verified = false;
    if (temp_valid && (!can_commit || can_commit(context)))
    {
        const bool had_old = fs.exists(filepath);
        if (had_old && fs.exists(backup_path) && valid_cached_jpeg_locked(fs, filepath))
            fs.remove(backup_path);
        const bool backed_up = !had_old ||
                               (backup_created = fs.rename(filepath, backup_path));
        if (transactional_replace_can_commit(size, bytesWritten, temp_valid, backed_up))
        {
            new_file_installed = (!can_commit || can_commit(context)) &&
                                 fs.rename(temp_path, filepath);
            commit_verified = new_file_installed && valid_cached_jpeg_locked(fs, filepath);
            if (commit_verified && can_commit && !can_commit(context))
                commit_verified = false;
            committed = commit_verified;
            if (commit_verified)
            {
                if (backup_created && fs.exists(backup_path)) fs.remove(backup_path);
            }
            else if (backup_created)
            {
                if (transactional_remove_new_final(new_file_installed, commit_verified) &&
                    fs.exists(filepath)) fs.remove(filepath);
                if (!fs.exists(filepath) && fs.exists(backup_path))
                    (void)fs.rename(backup_path, filepath);
            }
        }
    }
    // Keep a complete temp if commit/restore failed; recovery retries it later.
    if (fs.exists(temp_path) && !temp_valid) fs.remove(temp_path);
    sd_release_bus();

    if (committed)
        Serial.printf("[SD_CACHE] 💾 Đã commit cache: %s (%u bytes)\n", filepath, (unsigned)size);
    else
        Serial.printf("[SD_CACHE] ⚠️ Ghi cache thất bại; giữ bản cũ: %u/%u bytes\n",
                      (unsigned)bytesWritten, (unsigned)size);
    return committed;
}

bool sd_map_cache_write(double lat, double lon, int zoom, const char *maptype,
                        const uint8_t *in_buf, size_t size)
{
    return sd_map_cache_write_guarded(lat, lon, zoom, maptype, in_buf, size,
                                      nullptr, nullptr);
}

uint64_t sd_map_cache_get_free_mb(void)
{
    return storage_get_free_mb();
}
