/**
 * @file camera_app.cpp
 * @brief Giao diện Camera & RTSP Streamer responsive, tối ưu 240x320 portrait.
 */

#include "camera_app.h"
#include "../camera/camera_service.h"
#include "../display/lvgl_port.h"
#include "../ui/ui_theme.h"
#include <TJpg_Decoder.h>
#include "../display/tjpg_guard.h"
#include <esp_heap_caps.h>
#include <stdlib.h>

static_assert(sizeof(lv_color_t) == sizeof(uint16_t), "Camera preview requires LVGL RGB565");

// Các widget giao diện
static lv_obj_t *main_container = nullptr;
static lv_obj_t *cam_canvas = nullptr;
static lv_color_t *cam_canvas_buf = nullptr;
static uint16_t canvas_w = 224;
static uint16_t canvas_h = 188;
static bool camera_portrait = true;

static lv_obj_t *toolbar_box = nullptr;
static lv_obj_t *lbl_metrics = nullptr;
static lv_obj_t *lbl_cam_status = nullptr;
static lv_obj_t *btn_snap = nullptr;
static lv_obj_t *btn_cfg = nullptr;
static lv_obj_t *btn_disconnect = nullptr;

// Modal cấu hình & Bàn phím ảo
static lv_obj_t *cfg_modal = nullptr;
static lv_obj_t *cam_keyboard = nullptr;
static lv_obj_t *ta_name = nullptr;
static lv_obj_t *ta_ip = nullptr;
static lv_obj_t *ta_http_port = nullptr;
static lv_obj_t *ta_rtsp_port = nullptr;
static lv_obj_t *ta_onvif_port = nullptr;
static lv_obj_t *ta_user = nullptr;
static lv_obj_t *ta_pass = nullptr;
static lv_obj_t *dd_vendor = nullptr;
static lv_obj_t *dd_proto = nullptr;
static lv_obj_t *dd_security = nullptr;
static lv_obj_t *btn_save_connect = nullptr;

// Thống kê thực tế (True Telemetry)
static uint32_t last_rendered_frame_id = 0;
static uint32_t last_frame_time_ms = 0;
static uint32_t frame_count = 0;
static float real_fps = 0.0f;
static uint32_t last_fps_calc_time = 0;

static int16_t draw_offset_x = 0;
static int16_t draw_offset_y = 0;
static uint16_t *decode_target = nullptr;
static uint16_t *preview_front = nullptr;
static uint16_t *preview_back = nullptr;
static size_t preview_capacity_pixels = 0;
static SemaphoreHandle_t preview_mutex = nullptr;
static QueueHandle_t camera_command_queue = nullptr;
static TaskHandle_t camera_worker_handle = nullptr;
static volatile bool preview_active = false;
static uint32_t preview_frame_id = 0;
static uint32_t preview_timestamp_ms = 0;
static uint32_t preview_jpeg_bytes = 0;
static uint16_t preview_source_w = 0;
static uint16_t preview_source_h = 0;

enum CameraUiCommandType : uint8_t { CAM_UI_START, CAM_UI_STOP, CAM_UI_CONFIGURE };
struct CameraUiCommand
{
    CameraUiCommandType type;
    NetworkCameraProfile profile;
};

/* Callback của thư viện TJpgDec đưa dữ liệu RGB565 vào bộ đệm Canvas */
static bool camera_tjpg_output_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    if (!decode_target) return false;
    for (int16_t row = 0; row < h; row++)
    {
        int16_t dy = y + row + draw_offset_y;
        if (dy < 0) continue;
        if (dy >= canvas_h) break;
        for (int16_t col = 0; col < w; col++)
        {
            int16_t dx = x + col + draw_offset_x;
            if (dx < 0) continue;
            if (dx >= canvas_w) break;
            decode_target[dy * canvas_w + dx] = bitmap[row * w + col];
        }
    }
    return true;
}

/* Đọc thông số cấu hình từ giao diện UI và áp dụng vào Camera Service */
static bool enqueue_camera_command(const CameraUiCommand &command)
{
    return camera_worker_handle && camera_command_queue &&
           xQueueSend(camera_command_queue, &command, 0) == pdTRUE;
}

static bool queue_ui_configuration(void)
{
    NetworkCameraProfile prof;
    memset(&prof, 0, sizeof(prof));

    if (ta_name) strncpy(prof.name, lv_textarea_get_text(ta_name), sizeof(prof.name) - 1);
    if (ta_ip) strncpy(prof.ip, lv_textarea_get_text(ta_ip), sizeof(prof.ip) - 1);
    if (!prof.name[0]) strlcpy(prof.name, "IP Camera", sizeof(prof.name));
    char *port_end = nullptr;
    unsigned long parsed_port = ta_http_port ? strtoul(lv_textarea_get_text(ta_http_port), &port_end, 10) : 0;
    if (!prof.ip[0] || !port_end || *port_end != '\0' || parsed_port == 0 || parsed_port > 65535)
    {
        if (lbl_cam_status) lv_label_set_text(lbl_cam_status, "IP/hostname hoặc port không hợp lệ");
        return false;
    }
    prof.http_port = static_cast<uint16_t>(parsed_port);
    prof.rtsp_port = 0;
    prof.onvif_port = 0;
    if (ta_user) strncpy(prof.username, lv_textarea_get_text(ta_user), sizeof(prof.username) - 1);
    if (ta_pass) strncpy(prof.password, lv_textarea_get_text(ta_pass), sizeof(prof.password) - 1);

    if (dd_vendor)
    {
        uint16_t v_idx = lv_dropdown_get_selected(dd_vendor);
        switch (v_idx)
        {
            case 1: prof.vendor = CAM_VENDOR_HIKVISION; break;
            case 2: prof.vendor = CAM_VENDOR_KBVISION; break;
            case 3: prof.vendor = CAM_VENDOR_EZVIZ; break;
            case 4: prof.vendor = CAM_VENDOR_YOOSEE; break;
            case 0:
            default: prof.vendor = CAM_VENDOR_GENERIC_ONVIF; break;
        }
    }

    prof.protocol = CAM_PROTO_HTTP_SNAPSHOT;
    prof.security_mode = dd_security
        ? static_cast<CameraSecurityMode>(lv_dropdown_get_selected(dd_security))
        : CAM_SECURITY_TLS_VERIFIED;

    CameraUiCommand command = {};
    command.type = CAM_UI_CONFIGURE;
    command.profile = prof;
    if (!enqueue_camera_command(command))
    {
        if (lbl_cam_status) lv_label_set_text(lbl_cam_status, "COMMAND QUEUE FULL");
        return false;
    }
    return true;
}

// Bấm nút Refresh Snapshot
static void btn_snap_cb(lv_event_t *e)
{
    (void)e;
    CameraUiCommand command = {};
    command.type = CAM_UI_START;
    if (!enqueue_camera_command(command) && lbl_cam_status)
        lv_label_set_text(lbl_cam_status, "COMMAND QUEUE FULL");
}

// Bấm nút Mở / Đóng Cấu hình
static void btn_cfg_cb(lv_event_t *e)
{
    if (!cfg_modal) return;
    if (lv_obj_has_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_clear_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(cfg_modal);
    }
    else
    {
        if (cam_keyboard) lv_obj_add_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

// Bấm nút Ngắt kết nối
static void btn_disconnect_cb(lv_event_t *e)
{
    CameraUiCommand command = {};
    command.type = CAM_UI_STOP;
    if (!enqueue_camera_command(command) && lbl_cam_status)
        lv_label_set_text(lbl_cam_status, "COMMAND QUEUE FULL");
}

// Bấm Lưu & Kết nối trong Modal Cấu hình
static void btn_save_connect_cb(lv_event_t *e)
{
    if (cam_keyboard) lv_obj_add_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);
    bool applied = queue_ui_configuration();
    if (applied && cfg_modal)
    {
        lv_obj_add_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

// Callback khi focus vào ô nhập liệu
static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (cam_keyboard && ta)
    {
        lv_keyboard_set_textarea(cam_keyboard, ta);
        lv_obj_clear_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(cam_keyboard);
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    }
}

// Callback phím ảo bàn phím
static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    {
        if (cam_keyboard) lv_obj_add_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void decode_latest_frame(void)
{
    if (!preview_active || !preview_back || !preview_front) return;
    CameraFrame *frame = camera_service_get_frame(10);
    if (!frame) return;
    if (!frame->buf || frame->len == 0)
    {
        camera_service_return_frame(frame);
        return;
    }
    if (frame->frame_id == preview_frame_id)
    {
        camera_service_return_frame(frame);
        return;
    }

    memset(preview_back, 0, canvas_w * canvas_h * sizeof(uint16_t));
    uint16_t orig_w = 0;
    uint16_t orig_h = 0;
    uint8_t scale = 1;
    bool decoded = false;
    if (tjpg_guard_lock())
    {
        decode_target = preview_back;
        TJpgDec.setCallback(camera_tjpg_output_cb);
        TJpgDec.setSwapBytes(false);
        if (TJpgDec.getJpgSize(&orig_w, &orig_h, frame->buf, frame->len) == JDR_OK &&
            orig_w > 0 && orig_h > 0)
        {
            while (scale < 8 && ((orig_w / scale) > canvas_w || (orig_h / scale) > canvas_h))
                scale *= 2;
            TJpgDec.setJpgScale(scale);
            draw_offset_x = (static_cast<int16_t>(canvas_w) - static_cast<int16_t>(orig_w / scale)) / 2;
            draw_offset_y = (static_cast<int16_t>(canvas_h) - static_cast<int16_t>(orig_h / scale)) / 2;
            decoded = TJpgDec.drawJpg(0, 0, frame->buf, frame->len) == JDR_OK;
        }
        decode_target = nullptr;
        TJpgDec.setJpgScale(1);
        tjpg_guard_unlock();
    }

    if (decoded && preview_mutex && xSemaphoreTake(preview_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        uint16_t *old_front = preview_front;
        preview_front = preview_back;
        preview_back = old_front;
        preview_frame_id = frame->frame_id;
        preview_timestamp_ms = frame->timestamp_ms;
        preview_jpeg_bytes = frame->len;
        preview_source_w = orig_w;
        preview_source_h = orig_h;
        xSemaphoreGive(preview_mutex);
    }
    camera_service_return_frame(frame);
}

static void camera_ui_worker(void *)
{
    for (;;)
    {
        CameraUiCommand command = {};
        if (camera_command_queue && xQueueReceive(camera_command_queue, &command, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            if (command.type == CAM_UI_STOP)
            {
                (void)camera_service_stop(2000);
            }
            else if (command.type == CAM_UI_START)
            {
                (void)camera_service_start();
            }
            else if (command.type == CAM_UI_CONFIGURE)
            {
                if (camera_service_configure_network(command.profile))
                {
                    (void)camera_service_save_network_profile();
                    (void)camera_service_start();
                }
            }
        }
        decode_latest_frame();
    }
}

static bool ensure_camera_worker(void)
{
    if (!preview_mutex) preview_mutex = xSemaphoreCreateMutex();
    if (!camera_command_queue) camera_command_queue = xQueueCreate(4, sizeof(CameraUiCommand));
    if (!preview_mutex || !camera_command_queue) return false;
    if (!camera_worker_handle)
    {
        if (xTaskCreatePinnedToCore(camera_ui_worker, "CameraUiWorker", 6144, nullptr, 2,
                                    &camera_worker_handle, 0) != pdPASS)
        {
            camera_worker_handle = nullptr;
            return false;
        }
    }
    return true;
}

/* =========================================================================
 * KHỞI TẠO GIAO DIỆN CAMERA PORTRAIT
 * ========================================================================= */
void camera_app_open(lv_obj_t *parent)
{
    main_container = parent;
    lv_obj_set_style_pad_all(parent, 2, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    last_rendered_frame_id = 0;
    frame_count = 0;
    real_fps = 0.0f;
    last_fps_calc_time = millis();
    last_frame_time_ms = millis();

    // Preview phía trên, điều khiển và telemetry xếp dọc bên dưới.
    const bool portrait = SCREEN_WIDTH <= SCREEN_HEIGHT;
    camera_portrait = portrait;
    uint16_t toolbar_w = portrait ? (SCREEN_WIDTH - 4) : 96;
    canvas_w = portrait ? (SCREEN_WIDTH - 4) : (SCREEN_WIDTH - toolbar_w - 12);
    canvas_h = portrait ? 148 : (APP_CONTENT_HEIGHT - 6);
    const size_t required_pixels = static_cast<size_t>(canvas_w) * canvas_h;
    const bool worker_ready = ensure_camera_worker();

    if (preview_capacity_pixels < required_pixels)
    {
        uint16_t *new_front = static_cast<uint16_t *>(heap_caps_malloc(required_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        uint16_t *new_back = static_cast<uint16_t *>(heap_caps_malloc(required_pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!new_front) new_front = static_cast<uint16_t *>(malloc(required_pixels * sizeof(uint16_t)));
        if (!new_back) new_back = static_cast<uint16_t *>(malloc(required_pixels * sizeof(uint16_t)));
        if (new_front && new_back)
        {
            preview_front = new_front;
            preview_back = new_back;
            preview_capacity_pixels = required_pixels;
        }
        else
        {
            if (new_front) free(new_front);
            if (new_back) free(new_back);
        }
    }

    // CẤP PHÁT BỘ ĐỆM CANVAS TRONG PSRAM
    if (!cam_canvas_buf)
    {
        cam_canvas_buf = (lv_color_t *)heap_caps_malloc(canvas_w * canvas_h * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
        if (!cam_canvas_buf)
        {
            cam_canvas_buf = (lv_color_t *)malloc(canvas_w * canvas_h * sizeof(lv_color_t));
        }
    }

    if (cam_canvas_buf)
    {
        for (int i = 0; i < canvas_w * canvas_h; i++)
        {
            cam_canvas_buf[i] = lv_color_hex(0x0A0D14);
        }
    }
    if (preview_mutex && xSemaphoreTake(preview_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (preview_front) memset(preview_front, 0, required_pixels * sizeof(uint16_t));
        if (preview_back) memset(preview_back, 0, required_pixels * sizeof(uint16_t));
        preview_frame_id = 0;
        preview_active = worker_ready && preview_front && preview_back;
        xSemaphoreGive(preview_mutex);
    }

    // 2. Preview full-width phía trên.
    cam_canvas = lv_canvas_create(parent);
    if (cam_canvas_buf)
    {
        lv_canvas_set_buffer(cam_canvas, cam_canvas_buf, canvas_w, canvas_h, LV_IMG_CF_TRUE_COLOR);
    }
    lv_obj_set_size(cam_canvas, canvas_w, canvas_h);
    lv_obj_align(cam_canvas, portrait ? LV_ALIGN_TOP_MID : LV_ALIGN_LEFT_MID,
                 portrait ? 0 : 4, 0);
    lv_obj_set_style_border_color(cam_canvas, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(cam_canvas, 1, 0);
    lv_obj_set_style_radius(cam_canvas, 8, 0);

    // 3. Toolbar portrait bên dưới preview.
    toolbar_box = lv_obj_create(parent);
    lv_obj_set_size(toolbar_box, toolbar_w, portrait ? (APP_CONTENT_HEIGHT - canvas_h - 4) : canvas_h);
    lv_obj_align(toolbar_box, portrait ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_RIGHT_MID,
                 portrait ? 0 : -4, 0);
    lv_obj_set_style_bg_color(toolbar_box, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_border_color(toolbar_box, lv_color_hex(COLOR_CARD_BORDER), 0);
    lv_obj_set_style_border_width(toolbar_box, 1, 0);
    lv_obj_set_style_radius(toolbar_box, 10, 0);
    lv_obj_set_style_pad_all(toolbar_box, 4, 0);
    lv_obj_clear_flag(toolbar_box, LV_OBJ_FLAG_SCROLLABLE);

    uint16_t btn_w = portrait ? ((toolbar_w - 16) / 3) : (toolbar_w - 10);

    // Nút Snapshot (36px height)
    btn_snap = lv_btn_create(toolbar_box);
    lv_obj_set_size(btn_snap, btn_w, 36);
    lv_obj_set_ext_click_area(btn_snap, 4);
    if (portrait) lv_obj_set_pos(btn_snap, 2, 2);
    else lv_obj_align(btn_snap, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_radius(btn_snap, 6, 0);
    lv_obj_set_style_bg_color(btn_snap, lv_color_hex(COLOR_ACCENT_CYAN), 0);
    lv_obj_add_event_cb(btn_snap, btn_snap_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_snap = lv_label_create(btn_snap);
    lv_label_set_text(lbl_snap, LV_SYMBOL_REFRESH " Chụp");
    lv_obj_set_style_text_color(lbl_snap, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_text_font(lbl_snap, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_snap);

    // Nút Cấu hình (36px height)
    btn_cfg = lv_btn_create(toolbar_box);
    lv_obj_set_size(btn_cfg, btn_w, 36);
    lv_obj_set_ext_click_area(btn_cfg, 4);
    if (portrait) lv_obj_set_pos(btn_cfg, btn_w + 6, 2);
    else lv_obj_align(btn_cfg, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_radius(btn_cfg, 6, 0);
    lv_obj_set_style_bg_color(btn_cfg, lv_color_hex(0x1F2A3D), 0);
    lv_obj_set_style_border_color(btn_cfg, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_width(btn_cfg, 1, 0);
    lv_obj_add_event_cb(btn_cfg, btn_cfg_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_cfg = lv_label_create(btn_cfg);
    lv_label_set_text(lbl_cfg, LV_SYMBOL_SETTINGS " Cài");
    lv_obj_set_style_text_color(lbl_cfg, lv_color_hex(COLOR_TEXT_WHITE), 0);
    lv_obj_set_style_text_font(lbl_cfg, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_cfg);

    // Nút Dừng (36px height)
    btn_disconnect = lv_btn_create(toolbar_box);
    lv_obj_set_size(btn_disconnect, btn_w, 36);
    lv_obj_set_ext_click_area(btn_disconnect, 4);
    if (portrait) lv_obj_set_pos(btn_disconnect, btn_w * 2 + 10, 2);
    else lv_obj_align(btn_disconnect, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_radius(btn_disconnect, 6, 0);
    lv_obj_set_style_bg_color(btn_disconnect, lv_color_hex(0x281B24), 0);
    lv_obj_set_style_border_color(btn_disconnect, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_border_width(btn_disconnect, 1, 0);
    lv_obj_add_event_cb(btn_disconnect, btn_disconnect_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_dis = lv_label_create(btn_disconnect);
    lv_label_set_text(lbl_dis, LV_SYMBOL_POWER " Dừng");
    lv_obj_set_style_text_color(lbl_dis, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_set_style_text_font(lbl_dis, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_dis);

    // Dòng thông số thực tế (FPS decode thành công, kích thước, dung lượng, frame age)
    lbl_metrics = lv_label_create(toolbar_box);
    lv_label_set_text(lbl_metrics, portrait ? "FPS 0.0 • 0x0\n0 KB • Age 0 ms"
                                             : "FPS 0.0\n0x0\n0 KB\nAge 0 ms");
    lv_obj_set_width(lbl_metrics, portrait ? (toolbar_w - 12) : btn_w);
    lv_obj_set_style_text_color(lbl_metrics, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_set_style_text_font(lbl_metrics, UI_FONT_12, 0);
    lv_obj_align(lbl_metrics, LV_ALIGN_TOP_MID, 0, portrait ? 42 : 122);
    lv_obj_set_style_text_align(lbl_metrics, LV_TEXT_ALIGN_CENTER, 0);

    // Dòng trạng thái kết nối & Nguồn Camera
    lbl_cam_status = lv_label_create(toolbar_box);
    lv_label_set_text(lbl_cam_status, "READY");
    lv_obj_set_width(lbl_cam_status, portrait ? (toolbar_w - 12) : btn_w);
    lv_obj_set_style_text_color(lbl_cam_status, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_set_style_text_font(lbl_cam_status, UI_FONT_12, 0);
    lv_obj_align(lbl_cam_status, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_align(lbl_cam_status, LV_TEXT_ALIGN_CENTER, 0);

    // 4. MODAL CẤU HÌNH CAMERA CUỘN DỌC (MẶC ĐỊNH ẨN)
    cfg_modal = lv_obj_create(parent);
    lv_obj_set_size(cfg_modal, SCREEN_WIDTH - 4, APP_CONTENT_HEIGHT);
    lv_obj_center(cfg_modal);
    lv_obj_set_style_radius(cfg_modal, 12, 0);
    lv_obj_set_style_bg_color(cfg_modal, lv_color_hex(0x0C101A), 0);
    lv_obj_set_style_border_color(cfg_modal, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_border_width(cfg_modal, 1, 0);
    lv_obj_set_style_pad_all(cfg_modal, 6, 0);
    lv_obj_add_flag(cfg_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cfg_modal, LV_OBJ_FLAG_HIDDEN); // Ẩn ban đầu

    // Header modal cấu hình
    lv_obj_t *m_hdr = lv_label_create(cfg_modal);
    lv_label_set_text(m_hdr, LV_SYMBOL_SETTINGS " Cấu Hình IP Cam");
    lv_obj_set_style_text_color(m_hdr, lv_color_hex(COLOR_ACCENT_PURPLE), 0);
    lv_obj_set_style_text_font(m_hdr, UI_FONT_TITLE, 0);
    lv_obj_align(m_hdr, LV_ALIGN_TOP_LEFT, 4, 4);

    // Nút Đóng modal (Touch target >= 32x32)
    lv_obj_t *btn_m_close = lv_btn_create(cfg_modal);
    lv_obj_set_size(btn_m_close, 32, 32);
    lv_obj_set_ext_click_area(btn_m_close, 4);
    lv_obj_align(btn_m_close, LV_ALIGN_TOP_RIGHT, -2, 0);
    lv_obj_set_style_radius(btn_m_close, 6, 0);
    lv_obj_set_style_bg_color(btn_m_close, lv_color_hex(COLOR_ACCENT_RED), 0);
    lv_obj_add_event_cb(btn_m_close, btn_cfg_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_mc = lv_label_create(btn_m_close);
    lv_label_set_text(lbl_mc, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(lbl_mc, UI_FONT_12, 0);
    lv_obj_center(lbl_mc);

    const NetworkCameraProfile &cur_prof = camera_service_get_network_profile();

    // Helper tạo ô nhập liệu
    auto make_input = [&](const char *label_text, const char *default_val, lv_coord_t y_pos, bool is_pwd = false) -> lv_obj_t* {
        lv_obj_t *lbl = lv_label_create(cfg_modal);
        lv_label_set_text(lbl, label_text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
        lv_obj_set_pos(lbl, 6, y_pos);

        lv_obj_t *ta = lv_textarea_create(cfg_modal);
        lv_obj_set_size(ta, SCREEN_WIDTH - 28, 30);
        lv_obj_set_pos(ta, 6, y_pos + 14);
        lv_textarea_set_text(ta, default_val);
        lv_textarea_set_one_line(ta, true);
        if (is_pwd) lv_textarea_set_password_mode(ta, true);
        lv_obj_set_style_bg_color(ta, lv_color_hex(0x151B27), 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(0x2A354A), 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(COLOR_TEXT_WHITE), 0);
        lv_obj_set_style_text_font(ta, UI_FONT_SMALL, 0);
        lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
        return ta;
    };

    char buf_port[16];
    snprintf(buf_port, sizeof(buf_port), "%u", cur_prof.http_port > 0 ? cur_prof.http_port : 443);

    ta_name = make_input("Tên Camera:", cur_prof.name[0] ? cur_prof.name : "IP Camera", 36);
    ta_ip = make_input("IP hoặc hostname:", cur_prof.ip, 84);
    ta_http_port = make_input("HTTPS/HTTP Snapshot Port:", buf_port, 132);
    ta_rtsp_port = nullptr;
    ta_onvif_port = nullptr;
    ta_user = make_input("Tài khoản (nếu cần):", cur_prof.username, 180);
    ta_pass = make_input("Mật khẩu (chỉ giữ trong RAM):", cur_prof.password, 228, true);

    // Dropdown Hãng
    lv_obj_t *lbl_v = lv_label_create(cfg_modal);
    lv_label_set_text(lbl_v, "Nhà sản xuất (Vendor):");
    lv_obj_set_style_text_color(lbl_v, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_v, UI_FONT_SMALL, 0);
    lv_obj_set_pos(lbl_v, 6, 276);

    dd_vendor = lv_dropdown_create(cfg_modal);
    lv_obj_set_size(dd_vendor, SCREEN_WIDTH - 28, 30);
    lv_obj_set_pos(dd_vendor, 6, 292);
    lv_dropdown_set_options(dd_vendor, "Generic ONVIF\nHikvision\nKBVision\nEZVIZ\nYoosee");
    lv_dropdown_set_selected(dd_vendor, (uint16_t)cur_prof.vendor);
    lv_obj_set_style_bg_color(dd_vendor, lv_color_hex(0x151B27), 0);
    lv_obj_set_style_text_font(dd_vendor, UI_FONT_SMALL, 0);

    // Dropdown Giao thức
    lv_obj_t *lbl_p = lv_label_create(cfg_modal);
    lv_label_set_text(lbl_p, "Giao thức (Protocol):");
    lv_obj_set_style_text_color(lbl_p, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_p, UI_FONT_SMALL, 0);
    lv_obj_set_pos(lbl_p, 6, 328);

    dd_proto = lv_dropdown_create(cfg_modal);
    lv_obj_set_size(dd_proto, SCREEN_WIDTH - 28, 30);
    lv_obj_set_pos(dd_proto, 6, 344);
    lv_dropdown_set_options(dd_proto, "HTTP(S) Snapshot");
    lv_dropdown_set_selected(dd_proto, 0);
    lv_obj_add_state(dd_proto, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(dd_proto, lv_color_hex(0x151B27), 0);
    lv_obj_set_style_text_font(dd_proto, UI_FONT_SMALL, 0);

    lv_obj_t *lbl_security = lv_label_create(cfg_modal);
    lv_label_set_text(lbl_security, "Bảo mật transport:");
    lv_obj_set_style_text_color(lbl_security, lv_color_hex(COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_security, UI_FONT_SMALL, 0);
    lv_obj_set_pos(lbl_security, 6, 380);

    dd_security = lv_dropdown_create(cfg_modal);
    lv_obj_set_size(dd_security, SCREEN_WIDTH - 28, 30);
    lv_obj_set_pos(dd_security, 6, 396);
    lv_dropdown_set_options(dd_security,
        "HTTPS verified (CA required)\nHTTPS insecure - WARNING\nHTTP plaintext - WARNING");
    lv_dropdown_set_selected(dd_security, static_cast<uint16_t>(cur_prof.security_mode));
    lv_obj_set_style_bg_color(dd_security, lv_color_hex(0x151B27), 0);
    lv_obj_set_style_text_font(dd_security, UI_FONT_SMALL, 0);

    lv_obj_t *security_warning = lv_label_create(cfg_modal);
    lv_obj_set_width(security_warning, SCREEN_WIDTH - 28);
    lv_obj_set_pos(security_warning, 6, 430);
    lv_label_set_text(security_warning, "Insecure/HTTP là opt-in và có nguy cơ lộ credential/MITM.");
    lv_label_set_long_mode(security_warning, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(security_warning, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(security_warning, lv_color_hex(COLOR_ACCENT_RED), 0);

    // Nút Lưu & Kết nối (Touch target >= 32px)
    btn_save_connect = lv_btn_create(cfg_modal);
    lv_obj_set_size(btn_save_connect, SCREEN_WIDTH - 28, 34);
    lv_obj_set_ext_click_area(btn_save_connect, 4);
    lv_obj_set_pos(btn_save_connect, 6, 474);
    lv_obj_set_style_radius(btn_save_connect, 6, 0);
    lv_obj_set_style_bg_color(btn_save_connect, lv_color_hex(COLOR_ACCENT_GREEN), 0);
    lv_obj_add_event_cb(btn_save_connect, btn_save_connect_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl_save = lv_label_create(btn_save_connect);
    lv_label_set_text(lbl_save, LV_SYMBOL_SAVE " Lưu & Kết Nối");
    lv_obj_set_style_text_color(lbl_save, lv_color_hex(0x0A0D14), 0);
    lv_obj_set_style_text_font(lbl_save, UI_FONT_BUTTON, 0);
    lv_obj_center(lbl_save);

    // 5. BÀN PHÍM ẢO TOÀN CHIỀU RỘNG (MẶC ĐỊNH ẨN)
    cam_keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(cam_keyboard, SCREEN_WIDTH - 4, 118);
    lv_obj_align(cam_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(cam_keyboard, kb_event_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(cam_keyboard, LV_OBJ_FLAG_HIDDEN);

    CameraUiCommand start_command = {};
    start_command.type = CAM_UI_START;
    if (cur_prof.protocol != CAM_PROTO_HTTP_SNAPSHOT)
    {
        if (lbl_cam_status) lv_label_set_text(lbl_cam_status, "Protocol đã lưu không được hỗ trợ");
    }
    else if ((!preview_active || !enqueue_camera_command(start_command)) && lbl_cam_status)
    {
        lv_label_set_text(lbl_cam_status, "CAMERA/PREVIEW DEGRADED");
    }
}

/* Đóng và dọn dẹp */
void camera_app_close(void)
{
    if (preview_mutex && xSemaphoreTake(preview_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        preview_active = false;
        xSemaphoreGive(preview_mutex);
    }
    CameraUiCommand stop_command = {};
    stop_command.type = CAM_UI_STOP;
    if (!enqueue_camera_command(stop_command))
        Serial.println("[CAMERA_UI] Không thể gửi lệnh stop; service giữ trạng thái hiện tại");
    if (cam_canvas) lv_obj_del(cam_canvas);
    if (cam_canvas_buf)
    {
        free(cam_canvas_buf);
        cam_canvas_buf = nullptr;
    }
    main_container = nullptr;
    cam_canvas = nullptr;
    toolbar_box = nullptr;
    lbl_metrics = nullptr;
    lbl_cam_status = nullptr;
    btn_snap = nullptr;
    btn_cfg = nullptr;
    btn_disconnect = nullptr;
    cfg_modal = nullptr;
    cam_keyboard = nullptr;
    ta_name = nullptr;
    ta_ip = nullptr;
    ta_http_port = nullptr;
    ta_rtsp_port = nullptr;
    ta_onvif_port = nullptr;
    ta_user = nullptr;
    ta_pass = nullptr;
    dd_vendor = nullptr;
    dd_proto = nullptr;
    dd_security = nullptr;
    btn_save_connect = nullptr;
}

/* Cập nhật định kỳ */
void camera_app_update(void)
{
    if (!main_container || !cam_canvas || !cam_canvas_buf) return;
    uint32_t ready_frame_id = 0;
    uint32_t ready_timestamp_ms = 0;
    uint32_t ready_jpeg_bytes = 0;
    uint16_t ready_width = 0;
    uint16_t ready_height = 0;
    if (preview_mutex && xSemaphoreTake(preview_mutex, 0) == pdTRUE)
    {
        ready_frame_id = preview_frame_id;
        if (ready_frame_id != last_rendered_frame_id && preview_front)
        {
            memcpy(cam_canvas_buf, preview_front, static_cast<size_t>(canvas_w) * canvas_h * sizeof(lv_color_t));
            ready_timestamp_ms = preview_timestamp_ms;
            ready_jpeg_bytes = preview_jpeg_bytes;
            ready_width = preview_source_w;
            ready_height = preview_source_h;
            last_rendered_frame_id = ready_frame_id;
        }
        xSemaphoreGive(preview_mutex);
    }

    if (ready_frame_id != 0 && ready_frame_id == last_rendered_frame_id && ready_timestamp_ms != 0)
    {
        const uint32_t now = millis();
        frame_count++;
        if (now - last_fps_calc_time >= 1000)
        {
            real_fps = static_cast<float>(frame_count) * 1000.0f / (now - last_fps_calc_time);
            frame_count = 0;
            last_fps_calc_time = now;
        }
        const uint32_t age = now >= ready_timestamp_ms ? now - ready_timestamp_ms : 0;
        last_frame_time_ms = now;
        lv_obj_invalidate(cam_canvas);
        if (lbl_metrics)
        {
            if (camera_portrait)
                lv_label_set_text_fmt(lbl_metrics, "FPS %.1f • %ux%u\n%u KB • Age %u ms",
                    real_fps, ready_width, ready_height,
                    static_cast<unsigned>(ready_jpeg_bytes / 1024), static_cast<unsigned>(age));
            else
                lv_label_set_text_fmt(lbl_metrics, "FPS %.1f\n%ux%u\n%u KB\nAge %u ms",
                    real_fps, ready_width, ready_height,
                    static_cast<unsigned>(ready_jpeg_bytes / 1024), static_cast<unsigned>(age));
        }
    }

    if (lbl_cam_status)
    {
        lv_label_set_text(lbl_cam_status, camera_service_get_status_text());
    }
}
