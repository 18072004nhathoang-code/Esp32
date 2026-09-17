/**
 * @file storage_manager.h
 * @brief Phân hệ quản lý thẻ MicroSD SDMMC 4-bit/1-bit của ES3C28P.
 */

#pragma once

#include <Arduino.h>
#include <FS.h>
#include "board_config.h"

#include <SD_MMC.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo hệ thống lưu trữ thẻ nhớ theo profile phần cứng hiện tại
 * @return true nếu mount thẻ nhớ thành công
 */
bool storage_init(void);

/**
 * @brief Kiểm tra xem thẻ nhớ đã sẵn sàng sử dụng hay chưa
 */
bool storage_is_available(void);

/**
 * @brief Chiếm quyền truy cập thẻ nhớ (bảo vệ thread-safe / bus contention)
 */
bool storage_lock(uint32_t timeout_ms = 1000);

/**
 * @brief Giải phóng quyền truy cập thẻ nhớ
 */
void storage_unlock(void);

/**
 * @brief Lấy dung lượng tổng của thẻ nhớ (MB)
 */
uint64_t storage_get_total_mb(void);

/**
 * @brief Lấy dung lượng còn trống trên thẻ nhớ (MB)
 */
uint64_t storage_get_free_mb(void);

#ifdef __cplusplus
}

/**
 * @brief Lấy đối tượng file system của thẻ SDMMC.
 */
fs::FS& storage_get_fs(void);

#endif
