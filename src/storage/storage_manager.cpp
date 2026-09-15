/**
 * @file storage_manager.cpp
 * @brief Triển khai phân hệ lưu trữ thẻ nhớ MicroSD đa giao tiếp (SDMMC / SPI)
 */

#include "storage_manager.h"
#include "../display/spi_bus_guard.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static bool s_storage_ready = false;
static SemaphoreHandle_t s_storage_mutex = nullptr;

#if (BOARD_SD_INTERFACE == SD_IF_SPI)
static SPIClass s_sd_spi(FSPI);
#endif

bool storage_lock(uint32_t timeout_ms)
{
#if defined(BOARD_SD_SHARED_SPI) && (BOARD_SD_SHARED_SPI == true)
    return spi_bus_lock(timeout_ms);
#else
    if (!s_storage_mutex) return true;
    return (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
#endif
}

void storage_unlock(void)
{
#if defined(BOARD_SD_SHARED_SPI) && (BOARD_SD_SHARED_SPI == true)
    spi_bus_unlock();
#else
    if (s_storage_mutex)
    {
        xSemaphoreGive(s_storage_mutex);
    }
#endif
}

bool storage_init(void)
{
    if (s_storage_ready) return true;

    if (!s_storage_mutex)
    {
        s_storage_mutex = xSemaphoreCreateMutex();
    }

#if (BOARD_SD_INTERFACE == SD_IF_SPI)
    // --- KHỞI TẠO CHẾ ĐỘ SPI (DIYMORE 3.5" / DÙNG CHUNG FSPI) ---
    if (!storage_lock(1000))
    {
        Serial.println("[STORAGE] ❌ Không thể lock bus SPI để khởi tạo SD!");
        return false;
    }

    Serial.printf("[STORAGE] Khởi tạo thẻ SD qua SPI (SCK:%d, MISO:%d, MOSI:%d, CS:%d)...\n",
                  BOARD_SD_SCK, BOARD_SD_MISO, BOARD_SD_MOSI, BOARD_SD_CS);

    s_sd_spi.begin(BOARD_SD_SCK, BOARD_SD_MISO, BOARD_SD_MOSI, BOARD_SD_CS);
    if (!SD.begin(BOARD_SD_CS, s_sd_spi, 20000000))
    {
        if (!SD.begin(BOARD_SD_CS, s_sd_spi, 10000000))
        {
            Serial.println("[STORAGE] ⚠️ Không phát hiện thẻ nhớ SD qua giao diện SPI.");
            storage_unlock();
            s_storage_ready = false;
            return false;
        }
    }

    if (SD.cardType() == CARD_NONE)
    {
        Serial.println("[STORAGE] ⚠️ Không có thẻ nhớ trong khe cắm SPI!");
        storage_unlock();
        s_storage_ready = false;
        return false;
    }

    uint64_t total_mb = SD.cardSize() / (1024 * 1024);
    Serial.printf("[STORAGE] ✔ Thẻ MicroSD SPI sẵn sàng: %llu MB FAT32\n", total_mb);
    storage_unlock();
    s_storage_ready = true;
    return true;

#else
    // --- KHỞI TẠO CHẾ ĐỘ SDMMC / SDIO (ES3C28P / ĐỘC LẬP VỚI LCD) ---
    Serial.printf("[STORAGE] Khởi tạo thẻ SD qua SDMMC (CLK:%d, CMD:%d, D0:%d, D1:%d, D2:%d, D3:%d)...\n",
                  BOARD_SD_CLK, BOARD_SD_CMD, BOARD_SD_D0, BOARD_SD_D1, BOARD_SD_D2, BOARD_SD_D3);

    // Thử chế độ 4-bit High-speed trước
    SD_MMC.setPins(BOARD_SD_CLK, BOARD_SD_CMD, BOARD_SD_D0, BOARD_SD_D1, BOARD_SD_D2, BOARD_SD_D3);
    if (SD_MMC.begin("/sdcard", false /* 4-bit */, false, 20000))
    {
        uint64_t total_mb = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("[STORAGE] ✔ Thẻ MicroSD SDMMC 4-bit sẵn sàng: %llu MB FAT32\n", total_mb);
        s_storage_ready = true;
        return true;
    }

    // Fallback thử chế độ 1-bit nếu đường truyền 4-bit bị nhiễu
    Serial.println("[STORAGE] 4-bit mount thất bại, thử lại ở chế độ SDMMC 1-bit...");
    SD_MMC.setPins(BOARD_SD_CLK, BOARD_SD_CMD, BOARD_SD_D0);
    if (SD_MMC.begin("/sdcard", true /* 1-bit */, false, 20000))
    {
        uint64_t total_mb = SD_MMC.cardSize() / (1024 * 1024);
        Serial.printf("[STORAGE] ✔ Thẻ MicroSD SDMMC 1-bit sẵn sàng: %llu MB FAT32\n", total_mb);
        s_storage_ready = true;
        return true;
    }

    Serial.println("[STORAGE] ⚠️ Không phát hiện thẻ nhớ SD qua giao diện SDMMC.");
    s_storage_ready = false;
    return false;
#endif
}

bool storage_is_available(void)
{
    return s_storage_ready;
}

fs::FS& storage_get_fs(void)
{
#if (BOARD_SD_INTERFACE == SD_IF_SPI)
    return SD;
#else
    return SD_MMC;
#endif
}

uint64_t storage_get_total_mb(void)
{
    if (!s_storage_ready) return 0;
#if (BOARD_SD_INTERFACE == SD_IF_SPI)
    return SD.cardSize() / (1024 * 1024);
#else
    return SD_MMC.cardSize() / (1024 * 1024);
#endif
}

uint64_t storage_get_free_mb(void)
{
    if (!s_storage_ready) return 0;
#if (BOARD_SD_INTERFACE == SD_IF_SPI)
    return (SD.totalBytes() - SD.usedBytes()) / (1024 * 1024);
#else
    return (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
#endif
}
