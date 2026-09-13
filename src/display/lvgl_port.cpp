/**
 * @file lvgl_port.cpp
 * @brief Triển khai tầng kết nối LVGL 8 + LovyanGFX với FreeRTOS trên ESP32-S3
 */

#include "lvgl_port.h"
#include "../os/power_manager.h"
#include <esp_heap_caps.h>

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

/* Callback đẩy dữ liệu pixel từ LVGL sang màn hình bằng DMA qua LovyanGFX */
static void disp_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    gfx.startWrite();
    gfx.setAddrWindow(area->x1, area->y1, w, h);
    gfx.writePixelsDMA((uint16_t *)color_p, w * h);
    gfx.endWrite();

    // Báo cho LVGL biết frame đã hoàn thành để vẽ frame tiếp theo
    lv_disp_flush_ready(disp);
}

/* Callback đọc tọa độ cảm ứng từ LovyanGFX */
static void touchpad_read_cb(lv_indev_drv_t *indev, lv_indev_data_t *data)
{
    uint16_t touchX, touchY;
    bool touched = gfx.getTouch(&touchX, &touchY);

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

    while (1)
    {
        // Kiểm tra nếu hệ thống đang ở chế độ Sleep (tắt màn hình) để tạm dừng render LVGL
        if (power_manager_is_rendering_paused())
        {
            // Trong chế độ Sleep: Tạm dừng lv_timer_handler(), chỉ quét cảm ứng tiết kiệm điện để chờ Touch to Wake
            uint16_t touchX, touchY;
            if (gfx.getTouch(&touchX, &touchY))
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

bool lvgl_port_init(void)
{
    log_i("Khởi tạo phần cứng LovyanGFX...");
    if (!gfx.init())
    {
        log_e("Khởi tạo LovyanGFX thất bại!");
        return false;
    }

    // Xoay màn hình sang chế độ ngang (Landscape 320x240) chuẩn Mini OS
    gfx.setRotation(1);
    
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
    lv_disp_drv_register(&disp_drv);

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
