/**
 * @file main.cpp
 * @brief Điểm khởi chạy chính của hệ điều hành Mini OS trên ESP32-S3 2.8" Touch Display
 * Sử dụng VS Code + PlatformIO + LVGL 8 + LovyanGFX
 */

#include <Arduino.h>
#include "display/lvgl_port.h"
#include "ui/ui_manager.h"
#include "os/system_info.h"
#include "os/wifi_manager.h"
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
    Serial.println(" DIYMORE ESP32-S3 3.5\" IPS MINI OS (XiaoZhi AI Native) ");
    Serial.println("=======================================================");

    // In thông tin bộ nhớ thực tế nhận diện được
    SystemStats init_stats = system_get_stats();
    Serial.printf("[SYSTEM] Tần số CPU: %u MHz\n", init_stats.cpu_freq_mhz);
    Serial.printf("[SYSTEM] Flash Chip: %u MB\n", init_stats.flash_size_mb);
    Serial.printf("[SYSTEM] Total SRAM: %u KB (Free: %u KB)\n", init_stats.total_heap / 1024, init_stats.free_heap / 1024);
    Serial.printf("[SYSTEM] Total PSRAM: %u MB (Free: %u MB)\n", init_stats.total_psram / (1024 * 1024), init_stats.free_psram / (1024 * 1024));
    Serial.printf("[SYSTEM] Nhiệt độ khởi động: %.1f °C\n", init_stats.core_temp_c);

    // 2. Khởi tạo tầng đồ họa tăng tốc LovyanGFX + LVGL 8 qua FreeRTOS (Core 1)
    if (!lvgl_port_init())
    {
        Serial.println("[ERROR] Khởi tạo đồ họa thất bại! Vui lòng kiểm tra cấu hình chân trong LGFX_Config.hpp");
        while (1) { delay(1000); }
    }

    // 3. Khởi tạo dịch vụ mạng WiFi chạy nền trên Core 0
    Serial.println("[SYSTEM] Khởi tạo WiFi Manager Service trên Core 0...");
    wifi_manager_init();

    // 4. Khởi tạo hệ thống Âm thanh I2S Duplex (Mic MEMS & Loa ngoài) trên Core 0
    Serial.println("[SYSTEM] Khởi tạo I2S Audio Manager Service trên Core 0...");
    bool audio_ok = audio_manager_init();
    if (audio_ok)
    {
        audio_play_sound_effect(FX_CHIME); // Âm thanh khởi động Mini OS
    }
    else
    {
        Serial.println("[SYSTEM] ❌ Audio Manager khởi tạo thất bại! Tạm tắt các chức năng âm thanh.");
    }

    // 4b. Khởi tạo Music Player (ESP32-audioI2S & FreeRTOS Core 0)
    Serial.println("[SYSTEM] Khởi tạo Music Player Service trên Core 0...");
    bool music_ok = music_player_init();
    if (!music_ok)
    {
        Serial.println("[SYSTEM] ❌ Music Player khởi tạo thất bại! Vui lòng kiểm tra thẻ nhớ SD.");
    }

    // 4c. Khởi tạo AI Voice Assistant Service (Core 0)
    Serial.println("[SYSTEM] Khởi tạo AI Voice Assistant Service trên Core 0...");
    bool ai_voice_ok = ai_voice_init();
    if (!ai_voice_ok)
    {
        Serial.println("[SYSTEM] ❌ AI Voice Service khởi tạo thất bại!");
    }

    // 4d. Khởi tạo Camera Service đa nguồn (DVP / IP Camera)
    Serial.println("[SYSTEM] Khởi tạo Camera Service đa nguồn...");
    bool camera_ok = camera_service_init();
    if (!camera_ok)
    {
        Serial.println("[SYSTEM] ❌ Camera Service khởi tạo thất bại!");
    }

    // 5. Khởi tạo Desktop và các App hệ thống
    Serial.println("[GUI] Khởi tạo giao diện Desktop Mini OS...");
    ui_init();

    // 6. Tự động chuyển vào màn hình WiFi Settings App nếu chưa có mạng trong Flash NVS
    if (!wifi_manager_has_saved_credentials())
    {
        Serial.println("[SYSTEM] Chưa tìm thấy mạng WiFi trong NVS Flash! Tự động chuyển sang WiFi Settings App...");
        ui_open_wifi_app();
    }

    // 7. Khởi tạo Module Quản lý Nguồn & Tiết kiệm Năng lượng (Inactivity Timer 60s/120s & Touch to Wake)
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
