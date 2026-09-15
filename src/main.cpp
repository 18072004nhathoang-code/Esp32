/**
 * @file main.cpp
 * @brief Điểm khởi chạy chính của hệ điều hành Mini OS trên ESP32-S3 2.8" Touch Display
 * Sử dụng VS Code + PlatformIO + LVGL 8 + LovyanGFX
 */

#include <Arduino.h>
#include "board_config.h"
#include "shared_i2c_bus.h"
#include "display/lvgl_port.h"
#include "ui/ui_manager.h"
#include "os/system_info.h"
#include "os/wifi_manager.h"
#include "storage/storage_manager.h"
#include "audio/audio_manager.h"
#include "audio/music_player.h"
#include "ai/ai_voice_service.h"
#include "camera/camera_service.h"
#include "os/power_manager.h"

void setup()
{
    // 1. Khởi tạo Serial tốc độ cao (hỗ trợ USB CDC trên ESP32-S3)
    Serial.begin(115200);
    delay(1000); // Đợi ổn định cổng USB

    Serial.println("\n=======================================================");
    Serial.printf(" %s MINI OS \n", BOARD_PROFILE_NAME);
    Serial.println("=======================================================");

    // In thông tin phần cứng nhận diện thực tế
    SystemStats init_stats = system_get_stats();
    Serial.printf("[BOOT] MCU: %s @ %u MHz\n", BOARD_PROFILE_MCU, init_stats.cpu_freq_mhz);
    Serial.printf("[BOOT] Flash: %u MB (Target: %d MB) | PSRAM: %u MB (Target: %d MB)\n",
                  init_stats.flash_size_mb, BOARD_PROFILE_FLASH_MB,
                  init_stats.total_psram / (1024 * 1024), BOARD_PROFILE_PSRAM_MB);
    Serial.printf("[BOOT] Total SRAM: %u KB (Free: %u KB)\n", init_stats.total_heap / 1024, init_stats.free_heap / 1024);
    Serial.printf("[BOOT] Temperature: %.1f °C\n", init_stats.core_temp_c);
    Serial.printf("[HW] Board Profile: %s\n", BOARD_PROFILE_NAME);

    // 1b. [I2C] Khởi tạo physical I2C Bus dùng chung cho Touch & Audio Codec
    shared_i2c_init();

    // 2. [LCD] Khởi tạo tầng đồ họa LovyanGFX + LVGL 8 (Core 1)
    Serial.printf("[LCD] Panel: %s | Resolution: %dx%d | Bus: SPI 40MHz DMA\n",
#if (BOARD_LCD_CONTROLLER == LCD_CTRL_ILI9341)
                  "ILI9341V (2.8\" IPS)",
#elif (BOARD_LCD_CONTROLLER == LCD_CTRL_ST7796)
                  "ST7796 (3.5\" IPS)",
#else
                  "Generic LCD",
#endif
                  DISP_HOR_RES, DISP_VER_RES);

    if (!lvgl_port_init())
    {
        Serial.println("[LCD] ❌ Khởi tạo đồ họa thất bại! Vui lòng kiểm tra cấu hình chân!");
        while (1) { delay(1000); }
    }
    Serial.println("[LCD] Status: Ready (LVGL 8.3 + LovyanGFX DMA)");

    // 3. [TOUCH] Thông tin cảm ứng & Trạng thái Probe thật
    Serial.printf("[TOUCH] Controller: FT6336 Capacitive | I2C Addr: 0x%02X (Configured: SDA:%d, SCL:%d)\n",
                  BOARD_TOUCH_I2C_ADDR, BOARD_TOUCH_SDA, BOARD_TOUCH_SCL);
    if (shared_i2c_touch_is_detected())
    {
        Serial.println("[TOUCH] Status: Detected & Ready");
    }
    else
    {
        Serial.println("[TOUCH] Status: ⚠️ Not Detected (Degraded Mode, UI vẫn chạy)");
    }

    // 4. [SD] Khởi tạo phân hệ lưu trữ thẻ nhớ MicroSD qua HAL storage_manager
    Serial.printf("[SD] Interface: %s\n",
                  (BOARD_SD_INTERFACE == SD_IF_SPI) ? "SPI (FSPI)" : "SDMMC 4-bit (Hardware Host)");
    bool sd_ok = storage_init();
    if (sd_ok)
    {
        Serial.printf("[SD] Status: Ready (%llu MB FAT32)\n", storage_get_total_mb());
    }
    else
    {
        Serial.println("[SD] Status: ⚠️ Not Detected / Degraded Mode (Thẻ SD không sẵn sàng, hệ thống chạy Safe Mode)");
    }

    // 5. [AUDIO] Khởi tạo hệ thống Âm thanh I2S Duplex (Mic MEMS & Loa ngoài) trên Core 0
    Serial.printf("[AUDIO] Codec: ES8311 | I2C Addr: 0x%02X (Configured: SDA:%d, SCL:%d) | I2S (BCLK:%d, WS:%d, DOUT:%d, DIN:%d, MCLK:%d, PA:%d)\n",
                  BOARD_AUDIO_ES8311_ADDR, BOARD_AUDIO_I2C_SDA, BOARD_AUDIO_I2C_SCL,
                  AUDIO_I2S_BCLK, AUDIO_I2S_WS, AUDIO_I2S_DOUT, AUDIO_I2S_DIN, AUDIO_I2S_MCLK, AUDIO_PA_PIN);
    bool audio_ok = audio_manager_init();
    if (audio_ok)
    {
        if (shared_i2c_codec_is_detected())
        {
            Serial.println("[AUDIO] Status: Ready (ES8311 Codec Detected)");
        }
        else
        {
            Serial.println("[AUDIO] Status: Ready (Direct I2S / Bypass Mode)");
        }
        audio_play_sound_effect(FX_CHIME); // Âm thanh khởi động Mini OS
    }
    else
    {
        Serial.println("[AUDIO] Status: ⚠️ Audio Manager thất bại! Tiếp tục ở chế độ âm thanh giới hạn.");
    }

    // 5b. Khởi tạo Music Player (ESP32-audioI2S & FreeRTOS Core 0)
    bool music_ok = music_player_init();
    if (!music_ok)
    {
        Serial.println("[MUSIC] Status: ⚠️ Music Player degraded (Chưa nạp được danh sách nhạc thẻ nhớ).");
    }

    // 5c. Khởi tạo AI Voice Assistant Service (Core 0)
    bool ai_voice_ok = ai_voice_init();
    if (!ai_voice_ok)
    {
        Serial.println("[AI] Status: ⚠️ AI Voice Service khởi tạo thất bại!");
    }

    // 6. [WIFI] Khởi tạo dịch vụ mạng WiFi chạy nền trên Core 0
    Serial.println("[WIFI] Khởi tạo WiFi Manager Service trên Core 0...");
    wifi_manager_init();
    Serial.println("[WIFI] Status: Ready");

    // 7. [CAMERA] Khởi tạo Camera Service đa nguồn (DVP / IP Camera)
    bool camera_ok = camera_service_init();
    Serial.printf("[CAMERA] Status: %s\n", camera_service_get_status_text());

    // 8. Khởi tạo Desktop và các App hệ thống
    Serial.println("[GUI] Khởi tạo giao diện Desktop Mini OS...");
    ui_init();

    // 9. Tự động chuyển vào màn hình WiFi Settings App nếu chưa có mạng trong Flash NVS
    if (!wifi_manager_has_saved_credentials())
    {
        Serial.println("[SYSTEM] Chưa tìm thấy mạng WiFi trong NVS Flash! Tự động mở WiFi Settings App...");
        ui_open_wifi_app();
    }

    // 10. Khởi tạo Module Quản lý Nguồn & Tiết kiệm Năng lượng
    Serial.println("[SYSTEM] Khởi tạo Power Manager (60s Dimming -> 120s Sleep)...");
    power_manager_init();

    Serial.println("[SYSTEM] Mini OS Pro Max đã sẵn sàng hoạt động!");
}

void loop()
{
    static uint32_t last_tick = 0;

    // Cập nhật máy trạng thái nguồn (kiểm tra Inactivity Timer 60s/120s)
    power_manager_update();

    // Chu kỳ cập nhật dữ liệu nền của Mini OS (1 giây / lần)
    if (millis() - last_tick >= 1000)
    {
        last_tick = millis();

        // Thu thập thông số phần cứng
        SystemStats current_stats = system_get_stats();

        // Cập nhật lên thanh trạng thái và ứng dụng (Thread-Safe qua Mutex)
        ui_update_periodic(current_stats);

        // Kiểm tra sau khi khởi động 10s: nếu kết nối thất bại và chưa có Internet, tự động mở WiFi Settings App
        static bool boot_wifi_checked = false;
        if (!boot_wifi_checked && millis() > 10000)
        {
            boot_wifi_checked = true;
            if (!wifi_manager_is_connected())
            {
                Serial.println("[SYSTEM] Kết nối WiFi thất bại sau thời gian chờ. Tự động mở WiFi Settings App...");
                ui_open_wifi_app();
            }
        }
    }

    // Nhường CPU cho các tác vụ nền đúng cách qua FreeRTOS scheduler
    vTaskDelay(pdMS_TO_TICKS(50));
}
