/**
 * @file lvgl_port.cpp
 * @brief Triển khai tầng kết nối LVGL 8 + LovyanGFX với FreeRTOS trên ESP32-S3
 */

#include "lvgl_port.h"
#include "spi_bus_guard.h"
#include "../os/power_manager.h"
#include "../ui/fonts/ui_fonts.h"
#include "../ui/ui_theme.h"
#include "shared_i2c_bus.h"
#include <esp_heap_caps.h>
#include <Preferences.h>

// Khởi tạo đối tượng LovyanGFX toàn cục
LGFX gfx;

// Định nghĩa Semaphore Mutex bảo vệ tác vụ LVGL
SemaphoreHandle_t lvgl_mutex = nullptr;

// Bộ đệm vẽ của LVGL
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_buf1 = nullptr;
static lv_color_t *disp_buf2 = nullptr;

// Display & Input drivers
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// Task handle
static TaskHandle_t lvgl_task_handle = nullptr;
static uint8_t current_brightness = 85; // Mặc định 85%
static DisplayDiagnosticState display_diagnostic = {
    BOARD_LCD_RGB_ORDER, BOARD_LCD_INVERT
};
static const char *display_color_config_source = "BOARD_PROFILE";

static_assert(LV_COLOR_DEPTH == 16, "LVGL flush requires RGB565");
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

    // Đồng bộ bus SPI dùng chung với thẻ nhớ MicroSD
    if (spi_bus_lock(500))
    {
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
        gfx.waitDMA(); // Đảm bảo DMA hoàn tất truyền dữ liệu pixel trước khi nhả SPI bus
        if (swap_red_blue)
        {
            for (uint32_t i = 0; i < pixel_count; ++i)
            {
                uint16_t p = color_p[i].full;
                color_p[i].full = swap_red_blue_565(p);
            }
        }
        gfx.endWrite();
        spi_bus_unlock();
    }
    else
    {
        // Tuyệt đối không truy cập SPI khi lock thất bại để tránh xung đột với thẻ MicroSD
        // Bỏ qua frame hiện tại và báo cho LVGL tiếp tục chu trình tiếp theo
        Serial.println("[LVGL] Cảnh báo: spi_bus_lock() timeout trong disp_flush_cb, bỏ qua frame!");
    }

    // Báo cho LVGL biết hoàn tất lượt flush
    lv_disp_flush_ready(disp);
}

/* Callback đọc tọa độ cảm ứng từ LovyanGFX */
static void touchpad_read_cb(lv_indev_drv_t *indev, lv_indev_data_t *data)
{
    uint16_t touchX, touchY;
    bool touched = shared_i2c_touch_read(&touchX, &touchY);

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

static uint32_t diagnostic_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (software_red_blue_swap_required())
    {
        uint8_t tmp = red;
        red = blue;
        blue = tmp;
    }
    return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
}

static void run_lovyangfx_color_test(void)
{
    static const uint8_t levels[] = {0, 51, 102, 153, 204, 255};
    const int16_t band_h = gfx.height() / 4;
    gfx.startWrite();
    gfx.fillRect(0, 0, gfx.width() / 3, band_h, diagnostic_color(255, 0, 0));
    gfx.fillRect(gfx.width() / 3, 0, gfx.width() / 3, band_h, diagnostic_color(0, 255, 0));
    gfx.fillRect((gfx.width() / 3) * 2, 0, gfx.width() - (gfx.width() / 3) * 2, band_h,
                 diagnostic_color(0, 0, 255));
    gfx.fillRect(0, band_h, gfx.width() / 2, band_h, diagnostic_color(0, 0, 0));
    gfx.fillRect(gfx.width() / 2, band_h, gfx.width() - gfx.width() / 2, band_h,
                 diagnostic_color(255, 255, 255));
    const int16_t gray_w = gfx.width() / (int16_t)(sizeof(levels) / sizeof(levels[0]));
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i)
    {
        gfx.fillRect((int16_t)i * gray_w, band_h * 2,
                     i + 1 == sizeof(levels) / sizeof(levels[0]) ? gfx.width() - (int16_t)i * gray_w : gray_w,
                     gfx.height() - band_h * 2,
                     diagnostic_color(levels[i], levels[i], levels[i]));
    }
    gfx.endWrite();
    Serial.println("[DISPLAY_TEST] LovyanGFX direct: BLACK/WHITE RGB grayscale rendered");
    delay(250);
}

static void run_lvgl_color_test(lv_disp_t *display)
{
    static const uint32_t colors[] = {
        0xFF0000, 0x00FF00, 0x0000FF, 0x000000, 0xFFFFFF,
        0x333333, 0x666666, 0x999999, 0xCCCCCC
    };
    lv_obj_t *screen = lv_obj_create(nullptr);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    const lv_coord_t cell_w = DISP_HOR_RES / 3;
    const lv_coord_t cell_h = DISP_VER_RES / 3;
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i)
    {
        lv_obj_t *cell = lv_obj_create(screen);
        lv_obj_set_size(cell, i % 3 == 2 ? DISP_HOR_RES - cell_w * 2 : cell_w,
                        i / 3 == 2 ? DISP_VER_RES - cell_h * 2 : cell_h);
        lv_obj_set_pos(cell, (i % 3) * cell_w, (i / 3) * cell_h);
        lv_obj_set_style_bg_color(cell, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_radius(cell, 0, 0);
    }
    lv_scr_load(screen);
    lv_refr_now(display);
    Serial.println("[DISPLAY_TEST] LVGL flush: BLACK/WHITE RGB grayscale rendered");
    delay(250);
    lv_obj_clean(screen);
}

/* FreeRTOS Task chuyên trách render LVGL trên Core 1 */
static void lvgl_render_task(void *pvParameters)
{
    log_i("LVGL Task bắt đầu chạy trên Core %d", xPortGetCoreID());

    while (1)
    {
        // Kiểm tra nếu hệ thống đang ở chế độ Sleep (tắt màn hình) để tạm dừng render LVGL
        if (power_manager_is_rendering_paused())
        {
            // Trong chế độ Sleep: Tạm dừng lv_timer_handler(), chỉ quét cảm ứng tiết kiệm điện để chờ Touch to Wake
            uint16_t touchX, touchY;
            if (shared_i2c_touch_read(&touchX, &touchY))
            {
                // Chạm vào màn hình lúc đang ngủ -> đánh thức ngay lập tức!
                power_manager_wake();
            }

            // Nghỉ dài 50ms để giảm tải tối đa CPU trên Core 1
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (lvgl_port_lock(20))
        {
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
    if (!spi_bus_guard_init())
    {
        log_e("Không tạo được SPI bus guard!");
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
    run_lovyangfx_color_test();
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

    // Cấp phát 2 bộ đệm DMA trong Internal SRAM để đạt tốc độ SPI tối đa
    size_t buffer_size = DISP_HOR_RES * DISP_BUF_LINES * sizeof(lv_color_t);
    disp_buf1 = (lv_color_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    disp_buf2 = (lv_color_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    // Nếu không đủ SRAM cho 2 buffer, fallback về 1 buffer
    if (!disp_buf1)
    {
        log_w("Không đủ Internal SRAM DMA cho Buf1, thử malloc thông thường...");
        disp_buf1 = (lv_color_t *)malloc(buffer_size);
    }

    if (!disp_buf1)
    {
        log_e("Lỗi nghiêm trọng: Không thể cấp phát bộ nhớ đệm hiển thị!");
        return false;
    }

    // Cảnh báo nếu không đủ SRAM cho Double Buffer (FPS sẽ thấp hơn)
    if (!disp_buf2)
    {
        log_w("Không đủ Internal SRAM cho Buf2 - chạy ở chế độ Single Buffer (FPS thấp hơn).");
    }

    // Khởi tạo Draw Buffer (Hỗ trợ Double-Buffering nếu có disp_buf2)
    lv_disp_draw_buf_init(&draw_buf, disp_buf1, disp_buf2, DISP_HOR_RES * DISP_BUF_LINES);

    // Cấu hình Display Driver
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISP_HOR_RES;
    disp_drv.ver_res = DISP_VER_RES;
    disp_drv.flush_cb = disp_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_t *display = lv_disp_drv_register(&disp_drv);

    // Be Vietnam Pro is the default UI font. Its descriptor falls back to
    // Montserrat only for LVGL symbols absent from the Vietnamese font.
    lv_theme_t *theme = lv_theme_default_init(display,
                                               lv_palette_main(LV_PALETTE_CYAN),
                                               lv_palette_main(LV_PALETTE_BLUE),
                                               true,
                                               UI_FONT_BODY);
    lv_disp_set_theme(display, theme);

    run_lvgl_color_test(display);

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
        return false;
    }

    log_i("Hệ thống đồ họa LVGL + LovyanGFX khởi tạo thành công!");
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
