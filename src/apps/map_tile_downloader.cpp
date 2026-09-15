/**
 * @file map_tile_downloader.cpp
 * @brief Triển khai tác vụ nền tải ảnh bản đồ thực tế từ Google Maps Static API / OpenStreetMap,
 * kết hợp bộ nhớ đệm thẻ MicroSD 32GB FAT32 và giải mã JPEG bằng TJpgDec trong 8MB PSRAM.
 */

#include "map_tile_downloader.h"
#include "map_app.h"
#include "sd_map_cache.h"
#include "../os/wifi_manager.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <TJpg_Decoder.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Root CA certificates (Google Trust Services GTS Root R1, ISRG Root X1, GlobalSign R3)
static const char MAPS_TRUSTED_ROOT_CA_PEM[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4G\n"
    "A1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNp\n"
    "Z24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwMzE4\n"
    "MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzETMBEG\n"
    "A1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQYJKoZI\n"
    "hvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2EcWtiHL8\n"
    "RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUhhB5uzsT\n"
    "gHeMCOFJ0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL0gRgykmm\n"
    "KPZpO/bLyCiR5Z2KYVc3rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65TpjoWc4zd\n"
    "QQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjlOCZgdbKfd/+RFO+uIEn8rUAVSNECMWEZ\n"
    "XriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2xmmFghcCAwEAAaNCMEAw\n"
    "DgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFI/wS3+o\n"
    "LkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNvAUKr+yAzv95ZU\n"
    "RUm7lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8dEe3jgr25sbwMp\n"
    "jjM5RcOO5LlXbKr8EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw8lo/s7awlOqzJCK\n"
    "6fBdRoyV3XpYKBovHd7NADdBj+1EbddTKJd+82cEHhXXipa0095MJ6RMG3NzdvQX\n"
    "mcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18YIvDQVETI53O9zJrlAGomecs\n"
    "Mx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02JQZR7rkpeDMdmztcpH\n"
    "-----END CERTIFICATE-----\n";

// Kích thước bộ đệm thô (128 KB trong PSRAM cho tệp JPEG tải về)
#define JPEG_MAX_RAW_SIZE (128 * 1024)

// Cấu trúc yêu cầu tải ảnh bản đồ an toàn đa luồng
struct MapTileRequest
{
    double lat;
    double lon;
    int zoom;
    char maptype[16];
};

// Bộ đệm ảnh trong 8MB Octal PSRAM (Double-buffering chống tearing/race condition)
static uint8_t *jpeg_raw_buffer = nullptr;
static lv_color_t *tile_buf_front = nullptr; // Buffer hiển thị ổn định (Core 1 đọc)
static lv_color_t *tile_buf_back = nullptr;  // Buffer giải mã ngầm (Core 0 ghi)
static SemaphoreHandle_t tile_swap_mutex = nullptr;

// Quản lý trạng thái và đồng bộ đa luồng
static volatile TileDownloadStatus current_status = TILE_IDLE;
static volatile TileSource current_source = TILE_SOURCE_NONE;
static volatile bool has_new_tile = false;
static TaskHandle_t download_task_handle = nullptr;
static QueueHandle_t map_request_queue = nullptr;

// Khóa API đang hoạt động
static char active_api_key[128] = GOOGLE_MAPS_STATIC_API_KEY;

/* Callback của thư viện TJpgDec: Nhận khối điểm ảnh MCU (RGB565) và ghi vào tile_buf_back */
static bool tjpg_output_callback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    if (!tile_buf_back) return false;
    if (y >= MAP_TILE_HEIGHT) return false;

    for (int16_t row = 0; row < h; row++)
    {
        int16_t dest_y = y + row;
        if (dest_y >= MAP_TILE_HEIGHT) break;

        int16_t copy_w = w;
        if (x + copy_w > MAP_TILE_WIDTH) copy_w = MAP_TILE_WIDTH - x;
        if (copy_w <= 0) continue;

        memcpy(&tile_buf_back[dest_y * MAP_TILE_WIDTH + x], &bitmap[row * w], copy_w * sizeof(uint16_t));
    }
    return true;
}

/* Tác vụ nền chạy trên Core 0: Độc lập với Core 1 để tránh hoàn toàn hiện tượng giật/treo màn hình */
static void map_download_task(void *pvParameters)
{
    MapTileRequest req;

    while (1)
    {
        // Chờ nhận yêu cầu từ hàng đợi (chống race condition khi người dùng pan/zoom liên tục)
        if (map_request_queue && xQueueReceive(map_request_queue, &req, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            current_status = TILE_DOWNLOADING;

            double target_lat = req.lat;
            double target_lon = req.lon;
            int target_zoom = req.zoom;
            char target_type[16];
            strncpy(target_type, req.maptype, sizeof(target_type) - 1);
            target_type[sizeof(target_type) - 1] = '\0';

            // =========================================================================
            // BƯỚC 1: KIỂM TRA BỘ NHỚ ĐỆM TRÊN THẺ NHỚ MICROSD (CACHE HIT CHECK)
            // =========================================================================
            if (sd_map_cache_is_available() && sd_map_cache_exists(target_lat, target_lon, target_zoom, target_type))
            {
                Serial.println("[MAP_TASK] 🎯 Đang nạp ảnh trực tiếp từ thẻ MicroSD...");
                int bytes_read = sd_map_cache_read(target_lat, target_lon, target_zoom, target_type, jpeg_raw_buffer, JPEG_MAX_RAW_SIZE);
                if (bytes_read > 200)
                {
                    TJpgDec.setJpgScale(1);
                    TJpgDec.setSwapBytes(false); // Chuẩn RGB565 byte order cho LovyanGFX & LVGL 8
                    TJpgDec.setCallback(tjpg_output_callback);

                    JRESULT res = TJpgDec.drawJpg(0, 0, (const uint8_t *)jpeg_raw_buffer, bytes_read);
                    if (res == JDR_OK)
                    {
                        if (tile_swap_mutex && xSemaphoreTake(tile_swap_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                        {
                            lv_color_t *tmp = tile_buf_front;
                            tile_buf_front = tile_buf_back;
                            tile_buf_back = tmp;
                            current_source = TILE_SOURCE_SD_CACHE;
                            has_new_tile = true;
                            current_status = TILE_READY;
                            xSemaphoreGive(tile_swap_mutex);
                        }
                        Serial.println("[MAP_TASK] ✔ Nạp ảnh từ thẻ SD & giải mã thành công!");
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }
                    else
                    {
                        Serial.printf("[MAP_TASK] ⚠️ Lỗi giải mã ảnh từ thẻ SD: %d\n", res);
                    }
                }
            }

            // =========================================================================
            // BƯỚC 2: CACHE MISS -> TẢI MỚI TỪ GOOGLE MAPS STATIC API QUA INTERNET
            // =========================================================================
            if (!wifi_manager_is_connected())
            {
                Serial.println("[MAP_TASK] ⚠️ Không có kết nối WiFi để tải bản đồ!");
                current_status = TILE_ERROR;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // Xây dựng URL chuẩn Google Maps Static API kèm tham số bắt buộc solution_id
            char url_buf[512];
            if (strlen(active_api_key) > 5)
            {
                snprintf(url_buf, sizeof(url_buf),
                    "https://maps.googleapis.com/maps/api/staticmap?center=%.5f,%.5f&zoom=%d&size=%dx%d&scale=1&maptype=%s&format=jpg&key=%s&solution_id=%s",
                    target_lat, target_lon, target_zoom, MAP_TILE_WIDTH, MAP_TILE_HEIGHT, target_type, active_api_key, GMP_SOLUTION_ID);
            }
            else
            {
                // Nếu chưa cấu hình Key: Sử dụng máy chủ OpenStreetMap Static Map làm fallback
                snprintf(url_buf, sizeof(url_buf),
                    "https://staticmap.openstreetmap.de/staticmap.php?center=%.5f,%.5f&zoom=%d&size=%dx%d&maptype=%s",
                    target_lat, target_lon, target_zoom, MAP_TILE_WIDTH, MAP_TILE_HEIGHT,
                    (strcmp(target_type, "satellite") == 0 ? "satellite" : "mapnik"));
            }

            // Bảo mật: Không in khóa API plaintext ra log Serial
            Serial.printf("[MAP_TASK] 🌐 Tải bản đồ (Lat: %.4f, Lon: %.4f, Zoom: %d, Type: %s)\n",
                          target_lat, target_lon, target_zoom, target_type);

            HTTPClient http;
            WiFiClientSecure client;
            // Xác thực chứng chỉ TLS bảo mật qua Root CA bundle (GTS Root R1, ISRG Root X1, GlobalSign)
            client.setCACert(MAPS_TRUSTED_ROOT_CA_PEM);
            http.setTimeout(5000); // Giới hạn 5 giây timeout chống nghẽn tác vụ

            if (http.begin(client, url_buf))
            {
                int httpCode = http.GET();
                Serial.printf("[MAP_TASK] Mã phản hồi HTTP: %d\n", httpCode);

                if (httpCode == HTTP_CODE_OK)
                {
                    int total_len = http.getSize();
                    WiFiClient *stream = http.getStreamPtr();
                    int bytes_read = 0;

                    if (jpeg_raw_buffer != nullptr)
                    {
                        uint32_t start_read_time = millis();
                        while (http.connected() && (total_len > 0 || total_len == -1))
                        {
                            // Cơ chế chống treo máy: Timeout 6 giây khi đọc stream
                            if (millis() - start_read_time > 6000)
                            {
                                Serial.println("[MAP_TASK] ⚠️ Hết thời gian đọc luồng dữ liệu mạng!");
                                break;
                            }

                            size_t avail = stream->available();
                            if (avail > 0)
                            {
                                int read_size = avail;
                                if (bytes_read + read_size > JPEG_MAX_RAW_SIZE)
                                {
                                    read_size = JPEG_MAX_RAW_SIZE - bytes_read;
                                }
                                int r = stream->readBytes(&jpeg_raw_buffer[bytes_read], read_size);
                                if (r > 0)
                                {
                                    bytes_read += r;
                                    if (total_len > 0)
                                    {
                                        total_len -= r;
                                        if (total_len <= 0) break; // Đã nhận đủ dung lượng file
                                    }
                                }
                                if (bytes_read >= JPEG_MAX_RAW_SIZE) break;
                            }
                            else if (total_len == -1 && !http.connected())
                            {
                                break; // Stream chunked hoàn tất khi socket đóng
                            }
                            vTaskDelay(pdMS_TO_TICKS(2));
                        }

                        if (bytes_read > 200)
                        {
                            Serial.printf("[MAP_TASK] Đã tải về: %d bytes. Bắt đầu giải mã TJpgDec...\n", bytes_read);

                            TJpgDec.setJpgScale(1);
                            TJpgDec.setSwapBytes(false);
                            TJpgDec.setCallback(tjpg_output_callback);

                            JRESULT res = TJpgDec.drawJpg(0, 0, (const uint8_t *)jpeg_raw_buffer, bytes_read);
                            if (res == JDR_OK)
                            {
                                if (tile_swap_mutex && xSemaphoreTake(tile_swap_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
                                {
                                    lv_color_t *tmp = tile_buf_front;
                                    tile_buf_front = tile_buf_back;
                                    tile_buf_back = tmp;
                                    current_source = TILE_SOURCE_NETWORK;
                                    has_new_tile = true;
                                    current_status = TILE_READY;
                                    xSemaphoreGive(tile_swap_mutex);
                                }
                                Serial.println("[MAP_TASK] ✔ Giải mã thành công dữ liệu ảnh mạng!");

                                // =========================================================================
                                // BƯỚC 3: TỰ ĐỘNG GHI BẢN SAO JPEG VÀO THẺ NHỚ MICROSD (CACHE WRITE)
                                // =========================================================================
                                if (sd_map_cache_is_available())
                                {
                                    sd_map_cache_write(target_lat, target_lon, target_zoom, target_type, jpeg_raw_buffer, bytes_read);
                                }
                            }
                            else
                            {
                                Serial.printf("[MAP_TASK] ❌ Lỗi giải mã JPEG mạng: %d\n", res);
                                current_status = TILE_ERROR;
                            }
                        }
                        else
                        {
                            current_status = TILE_ERROR;
                        }
                    }
                }
                else
                {
                    current_status = TILE_ERROR;
                }
                http.end();
            }
            else
            {
                current_status = TILE_ERROR;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void map_tile_downloader_init(void)
{
    // 1. Khởi tạo bộ nhớ đệm thẻ nhớ MicroSD FAT32
    sd_map_cache_init();

    if (tile_swap_mutex == nullptr)
    {
        tile_swap_mutex = xSemaphoreCreateMutex();
    }

    // 2. Cấp phát bộ đệm thô và bộ đệm điểm ảnh kép (Front & Back) trong 8MB Octal PSRAM
    if (jpeg_raw_buffer == nullptr)
    {
        jpeg_raw_buffer = (uint8_t *)heap_caps_malloc(JPEG_MAX_RAW_SIZE, MALLOC_CAP_SPIRAM);
        if (!jpeg_raw_buffer)
        {
            jpeg_raw_buffer = (uint8_t *)malloc(JPEG_MAX_RAW_SIZE);
        }
    }

    size_t dec_size = MAP_TILE_WIDTH * MAP_TILE_HEIGHT * sizeof(lv_color_t);
    if (tile_buf_front == nullptr)
    {
        tile_buf_front = (lv_color_t *)heap_caps_malloc(dec_size, MALLOC_CAP_SPIRAM);
        if (!tile_buf_front) tile_buf_front = (lv_color_t *)malloc(dec_size);
    }
    if (tile_buf_back == nullptr)
    {
        tile_buf_back = (lv_color_t *)heap_caps_malloc(dec_size, MALLOC_CAP_SPIRAM);
        if (!tile_buf_back) tile_buf_back = (lv_color_t *)malloc(dec_size);
    }

    if (!jpeg_raw_buffer || !tile_buf_front || !tile_buf_back)
    {
        Serial.println("[MAP_TASK] ❌ Lỗi cấp phát bộ đệm kép PSRAM cho bản đồ!");
        return;
    }

    // 3. Khởi tạo hàng đợi FreeRTOS (Queue size 1 với overwrite)
    if (map_request_queue == nullptr)
    {
        map_request_queue = xQueueCreate(1, sizeof(MapTileRequest));
    }

    // 4. Khởi tạo Task FreeRTOS chạy ngầm trên Core 0 (Priority 2: Background network)
    if (download_task_handle == nullptr)
    {
        xTaskCreatePinnedToCore(
            map_download_task,
            "Map_Static_Task",
            8192,
            nullptr,
            2, // Priority 2
            &download_task_handle,
            0  // Pin Core 0 (Cách ly hoàn toàn khỏi LVGL Core 1)
        );
    }
}

void map_tile_downloader_request(double lat, double lon, int zoom, const char *maptype)
{
    MapTileRequest req;
    req.lat = lat;
    req.lon = lon;
    req.zoom = zoom;
    strncpy(req.maptype, (maptype && strlen(maptype) > 0) ? maptype : "roadmap", sizeof(req.maptype) - 1);
    req.maptype[sizeof(req.maptype) - 1] = '\0';

    if (map_request_queue)
    {
        xQueueOverwrite(map_request_queue, &req);
    }
}

bool map_tile_downloader_has_new_data(void)
{
    return has_new_tile;
}

const lv_color_t* map_tile_downloader_get_buffer(void)
{
    return tile_buf_front;
}

bool map_tile_downloader_copy_front(lv_color_t *dest, size_t count_pixels)
{
    if (!dest || !tile_buf_front) return false;
    if (tile_swap_mutex && xSemaphoreTake(tile_swap_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        size_t copy_rows = (MAP_CANVAS_HEIGHT < MAP_TILE_HEIGHT) ? MAP_CANVAS_HEIGHT : MAP_TILE_HEIGHT;
        size_t copy_cols = (MAP_CANVAS_WIDTH < MAP_TILE_WIDTH) ? MAP_CANVAS_WIDTH : MAP_TILE_WIDTH;
        for (size_t r = 0; r < copy_rows; r++)
        {
            memcpy(&dest[r * MAP_CANVAS_WIDTH], &tile_buf_front[r * MAP_TILE_WIDTH], copy_cols * sizeof(lv_color_t));
        }
        xSemaphoreGive(tile_swap_mutex);
        return true;
    }
    return false;
}

bool map_tile_downloader_consume_front(lv_color_t *dest, size_t count_pixels, TileSource *out_source)
{
    if (!dest || !tile_buf_front) return false;
    if (tile_swap_mutex && xSemaphoreTake(tile_swap_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (has_new_tile)
        {
            size_t copy_rows = (MAP_CANVAS_HEIGHT < MAP_TILE_HEIGHT) ? MAP_CANVAS_HEIGHT : MAP_TILE_HEIGHT;
            size_t copy_cols = (MAP_CANVAS_WIDTH < MAP_TILE_WIDTH) ? MAP_CANVAS_WIDTH : MAP_TILE_WIDTH;
            for (size_t r = 0; r < copy_rows; r++)
            {
                memcpy(&dest[r * MAP_CANVAS_WIDTH], &tile_buf_front[r * MAP_TILE_WIDTH], copy_cols * sizeof(lv_color_t));
            }
            if (out_source)
            {
                *out_source = current_source;
            }
            has_new_tile = false;
            xSemaphoreGive(tile_swap_mutex);
            return true;
        }
        xSemaphoreGive(tile_swap_mutex);
    }
    return false;
}

void map_tile_downloader_clear_new_data(void)
{
    has_new_tile = false;
}

TileDownloadStatus map_tile_downloader_get_status(void)
{
    return current_status;
}

TileSource map_tile_downloader_get_source(void)
{
    return current_source;
}

void map_tile_downloader_set_api_key(const char *key)
{
    if (key && strlen(key) > 0)
    {
        strncpy(active_api_key, key, sizeof(active_api_key) - 1);
        active_api_key[sizeof(active_api_key) - 1] = '\0';
    }
}
