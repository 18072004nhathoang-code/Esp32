/**
 * @file sd_map_cache.cpp
 * @brief Triển khai phân hệ bộ nhớ đệm bản đồ trên thẻ MicroSD FAT32 cho ESP32-S3
 */

#include "sd_map_cache.h"
#include "../display/lvgl_port.h"
#include "../display/spi_bus_guard.h"

static bool sd_initialized = false;
static SPIClass sd_spi(FSPI); // FSPI (SPI2_HOST) dùng chung GPIO 11/12/13

static bool sd_acquire_bus(uint32_t timeout_ms = 1000)
{
    return spi_bus_lock(timeout_ms);
}

static void sd_release_bus(void)
{
    spi_bus_unlock();
}

bool sd_map_cache_init(void)
{
    if (sd_initialized) return true;
    if (!sd_acquire_bus()) return false;

    Serial.println("[SD_CACHE] Đang khởi tạo thẻ nhớ MicroSD FAT32...");

    // Cấu hình các chân SPI cho thẻ MicroSD
    sd_spi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

    // Thử kết nối SD ở tần số 20MHz
    if (!SD.begin(SD_CS_PIN, sd_spi, 20000000))
    {
        // Thử lại ở tần số 10MHz nếu thẻ chậm hoặc đường dây dài
        if (!SD.begin(SD_CS_PIN, sd_spi, 10000000))
        {
            Serial.println("[SD_CACHE] ⚠️ Không tìm thấy thẻ nhớ MicroSD hoặc khởi tạo thất bại!");
            Serial.println("[SD_CACHE] -> Hệ thống sẽ hoạt động ở chế độ trực tiếp qua mạng (Direct Streaming).");
            sd_initialized = false;
            sd_release_bus();
            return false;
        }
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE)
    {
        Serial.println("[SD_CACHE] ⚠️ Không có thẻ nhớ trong khe cắm!");
        sd_initialized = false;
        sd_release_bus();
        return false;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("[SD_CACHE] ✔ Thẻ MicroSD sẵn sàng: %llu MB FAT32\n", cardSize);

    // Kiểm tra và tạo thư mục /maps nếu chưa có
    if (!SD.exists(SD_MAPS_DIR))
    {
        Serial.printf("[SD_CACHE] Tạo thư mục lưu trữ bản đồ: %s\n", SD_MAPS_DIR);
        if (!SD.mkdir(SD_MAPS_DIR))
        {
            Serial.println("[SD_CACHE] ⚠️ Không thể tạo thư mục /maps!");
        }
    }

    sd_initialized = true;
    sd_release_bus();
    return true;
}

bool sd_map_cache_is_available(void)
{
    return sd_initialized;
}

void sd_map_cache_get_filename(char *out_path, size_t max_len, double lat, double lon, int zoom, const char *maptype)
{
    if (!out_path || max_len == 0) return;
    const char *type = (maptype && strlen(maptype) > 0) ? maptype : "roadmap";
    snprintf(out_path, max_len, "%s/%.4f_%.4f_z%d_%s.jpg", SD_MAPS_DIR, lat, lon, zoom, type);
}

bool sd_map_cache_exists(double lat, double lon, int zoom, const char *maptype)
{
    if (!sd_initialized) return false;
    if (!sd_acquire_bus(200)) return false;

    char filepath[128];
    sd_map_cache_get_filename(filepath, sizeof(filepath), lat, lon, zoom, maptype);

    bool exists = SD.exists(filepath);
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
    if (!sd_initialized || !out_buf || max_size == 0) return -1;
    if (!sd_acquire_bus(500)) return -1;

    char filepath[128];
    sd_map_cache_get_filename(filepath, sizeof(filepath), lat, lon, zoom, maptype);

    File file = SD.open(filepath, FILE_READ);
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

    Serial.printf("[SD_CACHE] 📖 Đọc thành công %u bytes từ %s\n", (unsigned int)bytesRead, filepath);
    return (int)bytesRead;
}

bool sd_map_cache_write(double lat, double lon, int zoom, const char *maptype, const uint8_t *in_buf, size_t size)
{
    if (!sd_initialized || !in_buf || size == 0) return false;
    if (!sd_acquire_bus(500)) return false;

    char filepath[128];
    sd_map_cache_get_filename(filepath, sizeof(filepath), lat, lon, zoom, maptype);

    File file = SD.open(filepath, FILE_WRITE);
    if (!file)
    {
        Serial.printf("[SD_CACHE] ❌ Không thể mở tệp ghi: %s\n", filepath);
        sd_release_bus();
        return false;
    }

    size_t bytesWritten = file.write(in_buf, size);
    file.flush();
    file.close();
    sd_release_bus();

    if (bytesWritten == size)
    {
        Serial.printf("[SD_CACHE] 💾 Đã lưu cache thành công: %s (%u bytes)\n", filepath, (unsigned int)size);
        return true;
    }
    else
    {
        Serial.printf("[SD_CACHE] ⚠️ Ghi tệp không trọn vẹn: %u/%u bytes\n", (unsigned int)bytesWritten, (unsigned int)size);
        return false;
    }
}

uint64_t sd_map_cache_get_free_mb(void)
{
    if (!sd_initialized) return 0;
    if (!sd_acquire_bus(200)) return 0;
    uint64_t free_mb = (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024);
    sd_release_bus();
    return free_mb;
}
