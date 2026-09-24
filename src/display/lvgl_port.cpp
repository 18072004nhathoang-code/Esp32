/**
 * @file lvgl_port.cpp
 * @brief Triển khai tầng kết nối LVGL 8 + LovyanGFX với FreeRTOS trên ESP32-S3
 */

#include "lvgl_port.h"
#include "../os/power_manager.h"
#include "../ui/fonts/ui_fonts.h"
#include "../ui/ui_theme.h"
#include "../ai/ai_voice_service.h"
#include "shared_i2c_bus.h"
#include "../os/runtime_health.h"
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

// Khởi tạo đối tượng LovyanGFX toàn cục
LGFX gfx;

// Định nghĩa Semaphore Mutex bảo vệ tác vụ LVGL
SemaphoreHandle_t lvgl_mutex = nullptr;

// Bộ đệm vẽ của LVGL
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_buf1 = nullptr;

// Display & Input drivers
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// Task handle
static TaskHandle_t lvgl_task_handle = nullptr;
static LvglOwnerHook lvgl_owner_hook = nullptr;
static uint8_t current_brightness = 85; // Mặc định 85%
static DisplayDiagnosticState display_diagnostic = {
    BOARD_LCD_RGB_ORDER, BOARD_LCD_INVERT
};
static const char *display_color_config_source = "BOARD_PROFILE";

static_assert(LV_COLOR_DEPTH == 16, "LVGL flush requires RGB565");
static_assert(LV_COLOR_SCREEN_TRANSP == 1, "LVGL transform requires LV_COLOR_SCREEN_TRANSP 1");
static_assert(LV_COLOR_16_SWAP == 0, "LVGL RGB565 must use native byte order");
static_assert(sizeof(lv_color_t) == sizeof(lgfx::rgb565_t), "LVGL/LovyanGFX RGB565 size mismatch");
static_assert(alignof(lv_color_t) >= alignof(lgfx::rgb565_t), "LVGL/LovyanGFX RGB565 alignment mismatch");

static uint16_t swap_red_blue_565(uint16_t pixel)
{
    return (uint16_t)((pixel & 0x07E0U) | ((pixel & 0x001FU) << 11) | ((pixel & 0xF800U) >> 11));
}

static bool software_red_blue_swap_required(void)
{
    return display_diagnostic.bgr_order != (BOARD_LCD_RGB_ORDER != 0);
}

/* Callback đẩy dữ liệu pixel từ LVGL sang màn hình bằng DMA qua LovyanGFX */
static void disp_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    gfx.startWrite();
    gfx.setAddrWindow(area->x1, area->y1, w, h);
    uint32_t pixel_count = w * h;
    const bool swap_red_blue = software_red_blue_swap_required();
    if (swap_red_blue)
    {
        for (uint32_t i = 0; i < pixel_count; ++i)
        {
            uint16_t p = color_p[i].full;
            color_p[i].full = swap_red_blue_565(p);
        }
    }
    const lgfx::rgb565_t *pixels = reinterpret_cast<const lgfx::rgb565_t *>(color_p);
    gfx.writePixelsDMA(pixels, pixel_count);
    gfx.waitDMA();
    if (swap_red_blue)
    {
        for (uint32_t i = 0; i < pixel_count; ++i)
        {
            uint16_t p = color_p[i].full;
            color_p[i].full = swap_red_blue_565(p);
        }
    }
    gfx.endWrite();

    // Báo cho LVGL biết hoàn tất lượt flush
    lv_disp_flush_ready(disp);
}

/* Callback đọc tọa độ cảm ứng từ LovyanGFX */
static void touchpad_read_cb(lv_indev_drv_t *indev, lv_indev_data_t *data)
{
    uint16_t touchX = 0, touchY = 0;
    bool touched = shared_i2c_touch_read(&touchX, &touchY);

    SharedTouchSnapshot snap = {};
    const bool has_snap = shared_i2c_touch_get_snapshot(&snap);
    const bool io_ok = has_snap ? snap.io_ok : true;

    const AIVoiceState ai_state = ai_voice_get_state();
    if (ai_state == AI_STATE_STARTING || ai_state == AI_STATE_LISTENING)
    {
        static uint32_t last_log_ms = 0;
        static bool last_touched = false;
        static bool last_io_ok = true;
        const uint32_t now = millis();
        if (touched != last_touched || io_ok != last_io_ok || (now - last_log_ms) >= 500)
        {
            last_log_ms = now;
            last_touched = touched;
            last_io_ok = io_ok;
            log_i("Xiaozhi Touch: state=%s io_ok=%d x=%u y=%u ai_state=%d gen=%u",
                  touched ? "PR" : (io_ok ? "REL" : "I2C_ERR"),
                  io_ok ? 1 : 0,
                  static_cast<unsigned>(touchX), static_cast<unsigned>(touchY),
                  static_cast<int>(ai_state),
                  static_cast<unsigned>(ai_voice_get_active_generation()));
        }
    }

    if (touched)
    {
        // 1. Nếu màn hình đang ở trạng thái mờ (Dimmed 20%) hoặc ngủ (Sleep 0%)
        if (power_manager_get_state() != POWER_STATE_ACTIVE)
        {
            // Đánh thức màn hình sáng trở lại 100% ngay tức thì
            power_manager_wake();

            // Chặn sự kiện chạm này để không kích hoạt nút bấm bên dưới
            data->state = LV_INDEV_STATE_REL;
            return;
        }

        // 2. Nếu đang trong trạng thái chặn sau khi vừa đánh thức và người dùng chưa nhấc tay ra
        if (power_manager_should_suppress_touch())
        {
            data->state = LV_INDEV_STATE_REL;
            return;
        }

        // 3. Trạng thái bình thường (ACTIVE): Reset Inactivity Timer và gửi sự kiện nhấn
        power_manager_feed_activity();
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;
    }
    else
    {
        // Người dùng đã nhấc ngón tay ra khỏi màn hình -> giải phóng cờ chặn
        power_manager_clear_touch_suppression();
        data->state = LV_INDEV_STATE_REL;
    }
}

/* FreeRTOS Task chuyên trách render LVGL trên Core 1 */
static void lvgl_render_task(void *pvParameters)
{
    log_i("LVGL Task bắt đầu chạy trên Core %d", xPortGetCoreID());
    const bool watchdog_registered = esp_task_wdt_add(nullptr) == ESP_OK;
    if (!watchdog_registered) Serial.println("[LVGL] WARN: task watchdog registration failed");

    while (1)
    {
        if (watchdog_registered) esp_task_wdt_reset();
        runtime_health_heartbeat(RUNTIME_TASK_LVGL);
        // Kiểm tra nếu hệ thống đang ở chế độ Sleep (tắt màn hình) để tạm dừng render LVGL
        if (power_manager_is_rendering_paused())
        {
            // Trong chế độ Sleep: Tạm dừng lv_timer_handler(), chỉ quét cảm ứng tiết kiệm điện để chờ Touch to Wake
            uint16_t touchX = 0, touchY = 0;
            if (shared_i2c_touch_read(&touchX, &touchY))
            {
                // Chạm vào màn hình lúc đang ngủ -> đánh thức ngay lập tức!
                power_manager_wake();
            }

            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        if (lvgl_port_lock(20))
        {
            if (lvgl_owner_hook) lvgl_owner_hook();
            // lv_timer_handler tính toán animations, vẽ lại các widget cần cập nhật
            lv_timer_handler();
            lvgl_port_unlock();
        }
        
        // Nghỉ 5ms để nhường CPU cho các luồng khác (~60 - 100 FPS)
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    if (lvgl_mutex == nullptr) return false;
    const TickType_t ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(lvgl_mutex, ticks) == pdTRUE);
}

void lvgl_port_unlock(void)
{
    if (lvgl_mutex != nullptr)
    {
        xSemaphoreGiveRecursive(lvgl_mutex);
    }
}

const char* display_orientation_name(uint8_t rotation)
{
    switch (rotation)
    {
        case 0: return "PORTRAIT";
        case 1: return "LANDSCAPE";
        case 2: return "PORTRAIT_FLIPPED";
        case 3: return "LANDSCAPE_FLIPPED";
        default: return "UNKNOWN";
    }
}

bool lvgl_port_init(void)
{
    if (!psramFound() ||
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < (128U * 1024U) ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < (96U * 1024U))
    {
        log_e("LVGL requires working PSRAM with at least 128KB free");
        return false;
    }

    log_i("Khởi tạo phần cứng LovyanGFX...");
    if (!gfx.init())
    {
        log_e("Khởi tạo LovyanGFX thất bại!");
        return false;
    }

    // Cấu hình xoay màn hình theo profile board.
#if defined(BOARD_LCD_ROTATION)
    uint8_t rot = BOARD_LCD_ROTATION;
#else
    uint8_t rot = 0;
#endif
    gfx.setRotation(rot);
    Preferences display_prefs;
    if (display_prefs.begin("display_diag", true))
    {
        const bool has_bgr = display_prefs.isKey("bgr");
        const bool has_invert = display_prefs.isKey("invert");
        display_diagnostic.bgr_order = has_bgr ? display_prefs.getBool("bgr", BOARD_LCD_RGB_ORDER)
                                               : BOARD_LCD_RGB_ORDER;
        display_diagnostic.inverted = has_invert ? display_prefs.getBool("invert", BOARD_LCD_INVERT)
                                                  : BOARD_LCD_INVERT;
        if (has_bgr || has_invert) display_color_config_source = "NVS_BGR_INVERT";
        display_prefs.end();
    }
    gfx.invertDisplay(display_diagnostic.inverted);
    log_i("Display: %dx%d, Rotation: %d, Orientation: %s", DISP_HOR_RES, DISP_VER_RES, rot, display_orientation_name(rot));
    Serial.printf("Display: %dx%d\nRotation: %d\nOrientation: %s\n", DISP_HOR_RES, DISP_VER_RES, rot, display_orientation_name(rot));
    
    // Thiết lập độ sáng ban đầu
    lvgl_port_set_brightness(current_brightness);

    log_i("Khởi tạo thư viện đồ họa LVGL 8.3...");
    lv_init();

    // Tạo Recursive Mutex
    lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (lvgl_mutex == nullptr)
    {
        log_e("Không tạo được Mutex cho LVGL!");
        return false;
    }

    // Flush waits for DMA completion, so a second buffer only consumes internal RAM.
    size_t buffer_size = DISP_HOR_RES * DISP_BUF_LINES * sizeof(lv_color_t);
    disp_buf1 = (lv_color_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!disp_buf1)
    {
        log_e("Lỗi nghiêm trọng: Không thể cấp phát bộ nhớ đệm hiển thị!");
        vSemaphoreDelete(lvgl_mutex);
        lvgl_mutex = nullptr;
        return false;
    }

    lv_disp_draw_buf_init(&draw_buf, disp_buf1, nullptr, DISP_HOR_RES * DISP_BUF_LINES);

    // Cấu hình Display Driver (Giữ đầu ra RGB565, không bật screen_transp trên display driver)
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISP_HOR_RES;
    disp_drv.ver_res = DISP_VER_RES;
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.screen_transp = 0;
    lv_disp_t *display = lv_disp_drv_register(&disp_drv);
    lv_disp_set_bg_opa(display, LV_OPA_COVER);
    lv_disp_set_bg_color(display, lv_color_hex(COLOR_OS_BG));

    // Be Vietnam Pro is the default UI font. Its descriptor falls back to
    // Montserrat only for LVGL symbols absent from the Vietnamese font.
    lv_theme_t *theme = lv_theme_default_init(display,
                                               lv_palette_main(LV_PALETTE_CYAN),
                                               lv_palette_main(LV_PALETTE_BLUE),
                                               true,
                                               UI_FONT_BODY);
    lv_disp_set_theme(display, theme);

    // Cấu hình Touch Input Driver
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read_cb;
    lv_indev_drv_register(&indev_drv);

    // Tạo FreeRTOS Task chạy riêng trên Core 1 (ưu tiên độ mượt mà giao diện)
    // Stack 12KB: đủ cho canvas drawing, animation LVGL 8 và các widget phức tạp
    BaseType_t res = xTaskCreatePinnedToCore(
        lvgl_render_task,
        "LVGL_UI_Task",
        12 * 1024,          // 12KB Stack (tăng từ 8KB để đủ cho canvas & animation)
        nullptr,
        4,                  // Priority 4
        &lvgl_task_handle,
        1                   // Gắn chặt vào Core 1 của ESP32-S3
    );

    if (res != pdPASS)
    {
        log_e("Không thể khởi tạo FreeRTOS Task cho LVGL!");
        heap_caps_free(disp_buf1);
        disp_buf1 = nullptr;
        vSemaphoreDelete(lvgl_mutex);
        lvgl_mutex = nullptr;
        return false;
    }

    log_i("Hệ thống đồ họa LVGL + LovyanGFX khởi tạo thành công!");
    return true;

}

void lvgl_port_set_owner_hook(LvglOwnerHook hook)
{
    lvgl_owner_hook = hook;
}

bool lvgl_port_get_memory_stats(uint32_t *free_bytes, uint32_t *largest_free_bytes,
                                uint8_t *fragmentation_percent)
{
    if (!free_bytes || !largest_free_bytes || !fragmentation_percent) return false;
    lv_mem_monitor_t monitor = {};
    if (!lvgl_port_lock(50)) return false;
    lv_mem_monitor(&monitor);
    lvgl_port_unlock();
    *free_bytes = monitor.free_size;
    *largest_free_bytes = monitor.free_biggest_size;
    *fragmentation_percent = monitor.frag_pct;
    return true;
}

void lvgl_port_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    current_brightness = percent;
    uint8_t pwm_val = (uint32_t)percent * 255 / 100;
    gfx.setBrightness(pwm_val);
}

uint8_t lvgl_port_get_brightness(void)
{
    return current_brightness;
}

DisplayDiagnosticState lvgl_port_get_display_diagnostic(void)
{
    return display_diagnostic;
}

void lvgl_port_set_display_diagnostic(DisplayDiagnosticState state)
{
    display_diagnostic = state;
    gfx.invertDisplay(state.inverted);
    lv_obj_invalidate(lv_scr_act());
}

bool lvgl_port_apply_display_diagnostic(void)
{
    Preferences prefs;
    if (!prefs.begin("display_diag", false)) return false;
    prefs.remove("swap");
    bool ok = prefs.putBool("bgr", display_diagnostic.bgr_order) == 1;
    ok = (prefs.putBool("invert", display_diagnostic.inverted) == 1) && ok;
    prefs.end();
    if (ok) display_color_config_source = "NVS_BGR_INVERT";
    return ok;
}

const char* lvgl_port_get_color_config_source(void)
{
    return display_color_config_source;
}
