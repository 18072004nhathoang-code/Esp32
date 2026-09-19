/**
 * @file main.cpp
 * @brief Điểm khởi chạy chính của hệ điều hành Mini OS trên ESP32-S3 2.8" Touch Display
 * Sử dụng VS Code + PlatformIO + LVGL 8 + LovyanGFX
 */

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <esp_idf_version.h>
#include <esp_system.h>
#include "board_config.h"
#include "shared_i2c_bus.h"
#include "display/lvgl_port.h"
#include "ui/ui_manager.h"
#include "os/system_info.h"
#include "os/wifi_manager.h"
#include "os/time_service.h"
#include "storage/storage_manager.h"
#include "audio/audio_manager.h"
#include "audio/music_player.h"
#include "ai/ai_voice_service.h"
#include "camera/camera_service.h"
#include "os/power_manager.h"
#include "os/settings_service.h"
#include "firmware_regression.h"
#include "service_state_logic.h"

#ifndef FW_GIT_SHA
#define FW_GIT_SHA "unknown"
#endif

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason)
    {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_UNKNOWN:
        default: return "UNKNOWN";
    }
}

#ifdef MINI_OS_MUSIC_STRESS_TEST
static void music_stress_task(void *)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    const int track_count = music_player_get_track_count();
    const uint32_t baseline_heap = ESP.getFreeHeap();
    const UBaseType_t baseline_tasks = uxTaskGetNumberOfTasks();
    Serial.printf("[HW_STRESS] BEGIN duration=600s tracks=%d heap=%u tasks=%u\n",
                  track_count, static_cast<unsigned>(baseline_heap),
                  static_cast<unsigned>(baseline_tasks));
    if (track_count <= 0 || !music_player_play_index(0))
    {
        Serial.println("[HW_STRESS] FAIL no playable SD track");
        vTaskDelete(nullptr);
    }

    for (uint32_t second = 1; second <= 600; ++second)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t phase = second % 100U;
        if (phase == 20U) (void)music_player_next();
        else if (phase == 40U) (void)music_player_pause();
        else if (phase == 45U) (void)music_player_resume();
        else if (phase == 60U) (void)music_player_prev();
        else if (phase == 80U) (void)music_player_stop();
        else if (phase == 85U)
            (void)music_player_play_index(static_cast<int>((second / 100U) % track_count));

        if (second % 30U == 0)
        {
            Serial.printf("[HW_STRESS] t=%us playing=%s heap=%u tasks=%u\n",
                          static_cast<unsigned>(second),
                          music_player_is_playing() ? "YES" : "NO",
                          static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(uxTaskGetNumberOfTasks()));
        }
    }
    (void)music_player_stop();
    vTaskDelay(pdMS_TO_TICKS(3000));
    Serial.printf("[HW_STRESS] COMPLETE heap=%u delta=%d tasks=%u delta=%d\n",
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<int32_t>(ESP.getFreeHeap() - baseline_heap),
                  static_cast<unsigned>(uxTaskGetNumberOfTasks()),
                  static_cast<int>(uxTaskGetNumberOfTasks()) - static_cast<int>(baseline_tasks));
    vTaskDelete(nullptr);
}
#endif

void setup()
{
    // 1. Khởi tạo Serial tốc độ cao (hỗ trợ USB CDC trên ESP32-S3)
    Serial.begin(115200);
    delay(1000); // Đợi ổn định cổng USB

    Serial.println("\n=======================================================");
    Serial.printf(" %s MINI OS \n", BOARD_PROFILE_NAME);
    Serial.println("=======================================================");
    Serial.printf("[BOOT] Commit: %s\n", FW_GIT_SHA);
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    Serial.printf("[BOOT] Reset reason: %s (%d)\n",
                  reset_reason_name(reset_reason), static_cast<int>(reset_reason));
    Serial.printf("[BOOT] Framework: Arduino-ESP32 %u.%u.%u | ESP-IDF %s\n",
                  ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                  ESP_ARDUINO_VERSION_PATCH, esp_get_idf_version());
#if defined(CONFIG_MBEDTLS_HAVE_TIME_DATE) && CONFIG_MBEDTLS_HAVE_TIME_DATE
    Serial.println("[SECURITY] TLS certificate chain/hostname/date validation: ENABLED");
#else
    Serial.println("[SECURITY] TLS certificate chain/hostname validation: ENABLED; certificate date validation: UNAVAILABLE in this framework build");
#endif
    Serial.printf("[SELF_TEST] Firmware C++ contracts: %s\n",
                  firmware_regression_run() ? "PASS" : "FAIL");

    system_info_init();
    system_info_update();
    bool settings_ok = settings_service_init();
    MiniOsSettings saved_settings = settings_service_get();
    Serial.printf("[SETTINGS] Status: %s\n", settings_ok ? "Ready (NVS)" : settings_service_get_last_error());

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
                  "ILI9341V (2.8\" IPS)",
                  DISP_HOR_RES, DISP_VER_RES);

    if (!lvgl_port_init())
    {
        Serial.println("[LCD] ❌ Khởi tạo đồ họa thất bại! Vui lòng kiểm tra cấu hình chân!");
        while (1) { delay(1000); }
    }
    Serial.println("[LCD] Status: Ready (LVGL 8.3 + LovyanGFX DMA)");

    DisplayDiagnosticState display_state = lvgl_port_get_display_diagnostic();
    Serial.printf("[BOOT] Display: %dx%d rotation=%d (%s) pixel=RGB565 LV_COLOR_DEPTH=%d LV_COLOR_16_SWAP=%d\n",
                  DISP_HOR_RES, DISP_VER_RES, BOARD_LCD_ROTATION,
                  display_orientation_name(BOARD_LCD_ROTATION), LV_COLOR_DEPTH, LV_COLOR_16_SWAP);
    Serial.printf("[BOOT] Color config: source=%s order=%s inversion=%s byte_order=NATIVE\n",
                  lvgl_port_get_color_config_source(),
                  display_state.bgr_order ? "BGR" : "RGB",
                  display_state.inverted ? "ON" : "OFF");
    Serial.println("[BOOT] Touch mapping: raw -> invert X/Y -> rotation 2 -> full-screen identity; no calibration/NVS");

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
    Serial.println("[SD] Interface: SDMMC 4-bit (Hardware Host)");
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
        Serial.println("[AUDIO] Status: Ready (ES8311 configured + register readback passed)");
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
    bool wifi_ok = wifi_manager_init();
    wifi_manager_set_auto_reconnect(saved_settings.wifi_auto_reconnect);
    time_service_init();
    Serial.printf("[WIFI] Status: %s\n", wifi_ok ? "Ready" : "DEGRADED");

    // 7. Khởi tạo Module Quản lý Nguồn và áp dụng cấu hình đã lưu.
    power_manager_init();
    power_manager_set_timeouts(saved_settings.dim_timeout_sec, saved_settings.sleep_timeout_sec);
    power_manager_set_active_brightness(saved_settings.brightness);

    // 8. Khởi tạo Camera Service trước UI để Desktop thấy trạng thái thật ngay.
    bool camera_ok = camera_service_init();
    (void)camera_ok;
    Serial.printf("[CAMERA] Status: %s\n", camera_service_get_status_text());

    // 9. Khởi tạo Desktop sau khi các service nền đã có trạng thái thật.
    Serial.println("[GUI] Khởi tạo giao diện Desktop Mini OS...");
    ui_init();

    Serial.println("[SYSTEM] Mini OS Pro Max đã sẵn sàng hoạt động!");
#ifdef MINI_OS_MUSIC_STRESS_TEST
    TaskHandle_t stress_handle = nullptr;
    if (xTaskCreatePinnedToCore(music_stress_task, "MusicStress", 4096, nullptr, 1,
                                &stress_handle, 1) != pdPASS)
        Serial.println("[HW_STRESS] FAIL task allocation");
#endif
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
        system_info_update();
        time_service_update();
        SystemStats current_stats = system_get_stats();

        // Cập nhật lên thanh trạng thái và ứng dụng (Thread-Safe qua Mutex)
        ui_update_periodic(current_stats);

        // Kiểm tra sau khi khởi động 10s: chỉ tự động mở WiFi Settings App nếu CHƯA CÓ cấu hình mạng trong NVS
        static bool boot_wifi_prompted = false;
        if (!boot_wifi_prompted && millis() > 10000)
        {
            if (wifi_manager_is_connected())
                boot_wifi_prompted = true;
            else if (!wifi_manager_has_saved_credentials() &&
                     wifi_startup_may_open(true, false, ui_is_home_active(),
                                            ui_wifi_app_was_opened()))
            {
                boot_wifi_prompted = true;
                Serial.println("[SYSTEM] Chưa có WiFi lưu trong NVS. Mở WiFi Settings App...");
                ui_open_wifi_app();
            }
        }
    }

    // Nhường CPU cho các tác vụ nền đúng cách qua FreeRTOS scheduler
    vTaskDelay(pdMS_TO_TICKS(50));
}
