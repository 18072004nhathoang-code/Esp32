/**
 * @file storage_manager.cpp
 * @brief Triển khai phân hệ lưu trữ thẻ MicroSD SDMMC của ES3C28P.
 */

#include "storage_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static bool s_storage_ready = false;
static SemaphoreHandle_t s_storage_mutex = nullptr;

bool storage_lock(uint32_t timeout_ms)
{
    if (!s_storage_mutex) return true;
    return (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}

void storage_unlock(void)
{
    if (s_storage_mutex)
    {
        xSemaphoreGive(s_storage_mutex);
    }
}

bool storage_init(void)
{
    if (s_storage_ready) return true;

    if (!s_storage_mutex)
    {
        s_storage_mutex = xSemaphoreCreateMutex();
    }
    if (!s_storage_mutex)
    {
        Serial.println("[STORAGE] ❌ Chế độ suy giảm: không tạo được mutex lưu trữ");
        return false;
    }

    // ES3C28P dùng SDMMC / SDIO độc lập với màn hình.
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
}

bool storage_is_available(void)
{
    return s_storage_ready;
}

fs::FS& storage_get_fs(void)
{
    return SD_MMC;
}

uint64_t storage_get_total_mb(void)
{
    if (!s_storage_ready) return 0;
    return SD_MMC.cardSize() / (1024 * 1024);
}

uint64_t storage_get_free_mb(void)
{
    if (!s_storage_ready) return 0;
    return (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
}
