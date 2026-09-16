/**
 * @file map_app.cpp
 * @brief Triển khai ứng dụng xem bản đồ Google Maps trên ESP32-S3
 * Hỗ trợ Google Maps Static API (320x240, solution_id=gmp_git_agentskills_v1),
 * bộ nhớ đệm thẻ nhớ MicroSD FAT32, chuyển đổi Roadmap/Satellite và giao diện cảm ứng Zoom/Pan.
 */

#include "map_app.h"
#include "map_tile_downloader.h"
#include "sd_map_cache.h"
#include "../os/wifi_manager.h"
#include "../ui/ui_theme.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <math.h>

// Danh sách các địa điểm cài đặt sẵn
static const MapPresetLocation PRESETS[] = {
    {"Hà Nội (Hồ Gươm)",      21.0285, 105.8542, 15},
    {"TP.HCM (Bến Thành)",    10.7725, 106.6980, 15},
    {"Đà Nẵng (Cầu Rồng)",    16.0611, 108.2274, 15},
    {"Tokyo (Shibuya)",        35.6595, 139.7005, 15},
    {"Paris (Tháp Eiffel)",    48.8584,   2.2945, 15},
    {"New York (Times Sq)",    40.7580, -73.9855, 15}
};
static const size_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

// Trạng thái bản đồ
static double cur_lat = MAP_DEFAULT_LAT;
static double cur_lon = MAP_DEFAULT_LON;
static int cur_zoom = MAP_DEFAULT_ZOOM;
static size_t cur_preset_idx = 0;
static char cur_maptype[16] = "roadmap"; // "roadmap" hoặc "satellite"
static bool auto_fetch_enabled = true;
static uint8_t radar_anim_step = 0;

// Đối tượng giao diện LVGL
static lv_obj_t *app_container = nullptr;
static lv_obj_t *map_canvas = nullptr;
static lv_color_t *canvas_buffer = nullptr;
static lv_obj_t *hud_city_pill = nullptr;
static lv_obj_t *hud_lbl_city = nullptr;
static lv_obj_t *hud_source_pill = nullptr;
static lv_obj_t *hud_lbl_source = nullptr;
static lv_obj_t *hud_type_btn = nullptr;
static lv_obj_t *hud_lbl_type = nullptr;
static lv_obj_t *hud_coord_lbl = nullptr;

/* Hàm hỗ trợ vẽ đường thẳng trên lv_canvas */
static void draw_canvas_line(lv_obj_t *canvas, lv_point_t p1, lv_point_t p2, lv_color_t color, lv_coord_t width)
{
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.width = width;
    line_dsc.round_end = 1;
    line_dsc.round_start = 1;

    lv_point_t pts[2] = {p1, p2};
    lv_canvas_draw_line(canvas, pts, 2, &line_dsc);
}

/* Vẽ hình chữ nhật phủ màu */
static void draw_canvas_rect(lv_obj_t *canvas, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_color_t color)
{
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = color;
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.border_width = 0;
    lv_canvas_draw_rect(canvas, x, y, w, h, &rect_dsc);
}

/* Vẽ vòng tròn trên canvas */
static void draw_canvas_circle(lv_obj_t *canvas, lv_coord_t cx, lv_coord_t cy, lv_coord_t radius, lv_color_t color)
{
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = color;
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.radius = LV_RADIUS_CIRCLE;
    rect_dsc.border_width = 0;
    lv_canvas_draw_rect(canvas, cx - radius, cy - radius, radius * 2, radius * 2, &rect_dsc);
}

/* Vẽ các thành phần ghim Google Maps và Overlay */
static void draw_map_overlays(void)
{
    if (!map_canvas || !canvas_buffer) return;

    lv_coord_t center_x = MAP_CANVAS_WIDTH / 2;
    lv_coord_t center_y = MAP_CANVAS_HEIGHT / 2;

    // Sóng Radar phát xung
    radar_anim_step = (radar_anim_step + 1) % 4;
    lv_coord_t radar_r1 = 18 + radar_anim_step * 3;
    lv_coord_t radar_r2 = 12 + radar_anim_step * 2;
    draw_canvas_circle(map_canvas, center_x, center_y, radar_r1, lv_color_hex(0x3B1F27));
    draw_canvas_circle(map_canvas, center_x, center_y, radar_r2, lv_color_hex(0x5A2430));

    // Bóng đổ ghim
    draw_canvas_circle(map_canvas, center_x, center_y + 4, 6, lv_color_hex(0x0A0D14));
    // Đầu ghim đỏ Google (#FF3B30)
    draw_canvas_circle(map_canvas, center_x, center_y - 7, 9, lv_color_hex(0xFF3B30));
    // Đỉnh nhọn ghim
    draw_canvas_line(map_canvas, { (lv_coord_t)(center_x - 4), (lv_coord_t)(center_y - 4) }, { center_x, center_y }, lv_color_hex(0xFF3B30), 4);
    draw_canvas_line(map_canvas, { (lv_coord_t)(center_x + 4), (lv_coord_t)(center_y - 4) }, { center_x, center_y }, lv_color_hex(0xFF3B30), 4);
    // Điểm trắng trung tâm ghim
    draw_canvas_circle(map_canvas, center_x, center_y - 7, 3, lv_color_hex(0xFFFFFF));

    // La bàn góc trên trái dưới nút chế độ
    draw_canvas_circle(map_canvas, 24, 54, 12, lv_color_hex(0x1B2433));
    draw_canvas_line(map_canvas, { 24, 54 }, { 24, 44 }, lv_color_hex(0xFF3B30), 2);
    draw_canvas_line(map_canvas, { 24, 54 }, { 24, 64 }, lv_color_hex(0x718096), 2);

    // Thước đo khoảng cách (Scale Bar)
    draw_canvas_line(map_canvas, { 14, (lv_coord_t)(MAP_CANVAS_HEIGHT - 8) }, { 70, (lv_coord_t)(MAP_CANVAS_HEIGHT - 8) }, lv_color_hex(0xCBD5E0), 2);
    draw_canvas_line(map_canvas, { 14, (lv_coord_t)(MAP_CANVAS_HEIGHT - 13) }, { 14, (lv_coord_t)(MAP_CANVAS_HEIGHT - 8) }, lv_color_hex(0xCBD5E0), 2);
    draw_canvas_line(map_canvas, { 70, (lv_coord_t)(MAP_CANVAS_HEIGHT - 13) }, { 70, (lv_coord_t)(MAP_CANVAS_HEIGHT - 8) }, lv_color_hex(0xCBD5E0), 2);
}

/* Thuật toán vẽ bản đồ Vector Offline dự phòng khi chưa tải được ảnh */
static void render_offline_vector_map(void)
{
    if (!map_canvas || !canvas_buffer) return;

    bool is_sat = (strcmp(cur_maptype, "satellite") == 0);
    lv_color_t bg_col = is_sat ? lv_color_hex(0x0C1814) : lv_color_hex(0x181E29);
    lv_canvas_fill_bg(map_canvas, bg_col, LV_OPA_COVER);

    int scale = 1 << (cur_zoom > 10 ? (cur_zoom - 10) : 1);
    int offset_x = (int)(cur_lon * 120.0 * scale) % 45;
    int offset_y = (int)(cur_lat * 120.0 * scale) % 45;
    if (offset_x < 0) offset_x += 45;
    if (offset_y < 0) offset_y += 45;

    // Khối nhà đô thị
    for (int bx = offset_x + 10; bx < MAP_CANVAS_WIDTH - 20; bx += 55)
    {
        for (int by = offset_y + 10; by < MAP_CANVAS_HEIGHT - 20; by += 45)
        {
            lv_color_t block_col = is_sat ? lv_color_hex(0x162820) : lv_color_hex(0x222B38);
            draw_canvas_rect(map_canvas, bx, by, 22, 16, block_col);
            draw_canvas_rect(map_canvas, bx + 26, by, 18, 16, block_col);
        }
    }

    // Công viên / Thảm thực vật
    lv_color_t green_col = is_sat ? lv_color_hex(0x0F2E1E) : lv_color_hex(0x1A362B);
    draw_canvas_rect(map_canvas, (offset_x + 15) % MAP_CANVAS_WIDTH, 12, 75, 55, green_col);
    draw_canvas_rect(map_canvas, (offset_x + 150) % MAP_CANVAS_WIDTH, 90, 60, 45, green_col);

    // Sông hồ
    for (int y = 0; y < MAP_CANVAS_HEIGHT; y += 4)
    {
        int river_x = (int)(sinf((y + offset_y * 2) * 0.05f) * 22.0f) + (MAP_CANVAS_WIDTH / 2) + offset_x - 35;
        if (river_x >= 0 && river_x < MAP_CANVAS_WIDTH - 25)
        {
            draw_canvas_rect(map_canvas, river_x, y, 22, 4, lv_color_hex(0x13273D));
        }
    }

    // Đại lộ / Đường chính
    int hwy_y = (MAP_CANVAS_HEIGHT / 2) + (offset_y % 35) - 18;
    draw_canvas_line(map_canvas, { 0, (lv_coord_t)hwy_y }, { MAP_CANVAS_WIDTH, (lv_coord_t)(hwy_y + 24) }, lv_color_hex(0xEA8E18), 3);
}

/**
 * @brief Render nội dung bản đồ lên Canvas.
 * @note Hàm này phải được gọi trong khi đang giữ lvgl_mutex hoặc từ LVGL event callback.
 */
void map_app_render(void)
{
    // Tiêu thụ nguyên tử frame mới từ Front Buffer dưới Mutex
    TileSource src = TILE_SOURCE_NONE;
    if (canvas_buffer && map_tile_downloader_consume_front(canvas_buffer, MAP_CANVAS_WIDTH * MAP_CANVAS_HEIGHT, &src))
    {
        if (hud_lbl_source)
        {
            if (src == TILE_SOURCE_SD_CACHE)
            {
                lv_label_set_text(hud_lbl_source, LV_SYMBOL_SD_CARD " SD Cache Hit");
                lv_obj_set_style_text_color(hud_lbl_source, lv_color_hex(0x00E676), 0);
            }
            else
            {
                lv_label_set_text(hud_lbl_source, LV_SYMBOL_WIFI " Google Static API");
                lv_obj_set_style_text_color(hud_lbl_source, lv_color_hex(0x00F2FE), 0);
            }
        }
        draw_map_overlays();
    }
    else
    {
        TileDownloadStatus st = map_tile_downloader_get_status();
        if (st == TILE_DOWNLOADING)
        {
            if (hud_lbl_source)
            {
                lv_label_set_text(hud_lbl_source, LV_SYMBOL_REFRESH " Đang tải ảnh...");
                lv_obj_set_style_text_color(hud_lbl_source, lv_color_hex(0xF39C12), 0);
            }
        }
        else if (st == TILE_ERROR)
        {
            if (hud_lbl_source)
            {
                lv_label_set_text(hud_lbl_source, "[!] Lỗi nạp -> Vector");
                lv_obj_set_style_text_color(hud_lbl_source, lv_color_hex(0xFF3B30), 0);
            }
            render_offline_vector_map();
            draw_map_overlays();
        }
    }

    // Cập nhật nhãn thành phố & Zoom
    if (hud_lbl_city)
    {
        lv_label_set_text_fmt(hud_lbl_city, "%s • Z%d", PRESETS[cur_preset_idx].name, cur_zoom);
    }
    if (hud_coord_lbl)
    {
        lv_label_set_text_fmt(hud_coord_lbl, "%.4f, %.4f", cur_lat, cur_lon);
    }
    if (hud_lbl_type)
    {
        if (strcmp(cur_maptype, "satellite") == 0)
        {
            lv_label_set_text(hud_lbl_type, "Vệ Tinh");
            lv_obj_set_style_text_color(hud_lbl_type, lv_color_hex(0x00F2FE), 0);
        }
        else
        {
            lv_label_set_text(hud_lbl_type, "Đường Phố");
            lv_obj_set_style_text_color(hud_lbl_type, lv_color_hex(0x00E676), 0);
        }
    }

    if (map_canvas)
    {
        lv_obj_invalidate(map_canvas);
    }
}

static void trigger_map_reload(void)
{
    if (hud_lbl_source)
    {
        // Kiểm tra nhanh trước trên thẻ nhớ SD để cập nhật nhãn tức thì
        if (sd_map_cache_is_available() && sd_map_cache_exists(cur_lat, cur_lon, cur_zoom, cur_maptype))
        {
            lv_label_set_text(hud_lbl_source, LV_SYMBOL_SD_CARD " Đang đọc thẻ SD...");
            lv_obj_set_style_text_color(hud_lbl_source, lv_color_hex(0x00E676), 0);
        }
        else if (wifi_manager_is_connected())
        {
            lv_label_set_text(hud_lbl_source, LV_SYMBOL_WIFI " Gửi yêu cầu Google API...");
            lv_obj_set_style_text_color(hud_lbl_source, lv_color_hex(0x00F2FE), 0);
        }
        else
        {
            lv_label_set_text(hud_lbl_source, "[Offline] Chưa có cache");
            lv_obj_set_style_text_color(hud_lbl_source, lv_color_hex(0xA0AEC0), 0);
        }
    }

    map_tile_downloader_request(cur_lat, cur_lon, cur_zoom, cur_maptype);
    map_app_render();
}

void map_app_zoom_in(void)
{
    if (cur_zoom < 20)
    {
        cur_zoom++;
        trigger_map_reload();
    }
}

void map_app_zoom_out(void)
{
    if (cur_zoom > 5)
    {
        cur_zoom--;
        trigger_map_reload();
    }
}

void map_app_pan_direction(uint8_t dir)
{
    // Bước dịch chuyển thích ứng theo mức Zoom
    double step = 0.006 / (1 << (cur_zoom > 12 ? (cur_zoom - 12) : 1));

    switch (dir)
    {
        case 1: cur_lat += step; break; // Lên (Bắc)
        case 2: cur_lat -= step; break; // Xuống (Nam)
        case 3: cur_lon -= step; break; // Trái (Tây)
        case 4: cur_lon += step; break; // Phải (Đông)
    }
    trigger_map_reload();
}

void map_app_toggle_map_type(void)
{
    if (strcmp(cur_maptype, "roadmap") == 0)
    {
        strcpy(cur_maptype, "satellite");
    }
    else
    {
        strcpy(cur_maptype, "roadmap");
    }
    trigger_map_reload();
}

const char* map_app_get_current_type(void)
{
    return cur_maptype;
}

/* Event callbacks */
static void btn_zoom_in_cb(lv_event_t *e)  { map_app_zoom_in(); }
static void btn_zoom_out_cb(lv_event_t *e) { map_app_zoom_out(); }

static void btn_pan_cb(lv_event_t *e)
{
    uintptr_t dir = (uintptr_t)lv_event_get_user_data(e);
    map_app_pan_direction((uint8_t)dir);
}

static void btn_toggle_type_cb(lv_event_t *e)
{
    map_app_toggle_map_type();
}

static void btn_next_city_cb(lv_event_t *e)
{
    cur_preset_idx = (cur_preset_idx + 1) % PRESET_COUNT;
    cur_lat = PRESETS[cur_preset_idx].lat;
    cur_lon = PRESETS[cur_preset_idx].lon;
    cur_zoom = PRESETS[cur_preset_idx].zoom;
    trigger_map_reload();
}

void map_app_open(lv_obj_t *parent)
{
    app_container = parent;
    lv_obj_clean(parent);
    lv_obj_set_style_pad_all(parent, 0, 0);

    // 1. Cấp phát bộ đệm Canvas trong 8MB PSRAM (320x240 x 2 byte)
    if (canvas_buffer == nullptr)
    {
        size_t buf_size = MAP_CANVAS_WIDTH * MAP_CANVAS_HEIGHT * sizeof(lv_color_t);
        canvas_buffer = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (!canvas_buffer)
        {
            canvas_buffer = (lv_color_t *)malloc(buf_size);
        }
    }

    if (!canvas_buffer)
    {
        lv_obj_t *err_lbl = lv_label_create(parent);
        lv_label_set_text(err_lbl, "Lỗi: Không đủ bộ nhớ PSRAM cấp phát Canvas!");
        lv_obj_center(err_lbl);
        return;
    }

    // 2. Khởi tạo tác vụ tải ảnh nền và thẻ MicroSD FAT32
    map_tile_downloader_init();

    // 3. Tạo Canvas tràn viền MAP_CANVAS_WIDTH x MAP_CANVAS_HEIGHT
    map_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(map_canvas, canvas_buffer, MAP_CANVAS_WIDTH, MAP_CANVAS_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(map_canvas, 0, 0);
    lv_obj_set_size(map_canvas, MAP_CANVAS_WIDTH, MAP_CANVAS_HEIGHT);

    // 4. FLOATING HUD TOP: Nút chuyển đổi kiểu bản đồ (Roadmap / Satellite)
    hud_type_btn = lv_btn_create(parent);
    lv_obj_set_size(hud_type_btn, 46, 24);
    lv_obj_set_ext_click_area(hud_type_btn, 6);
    lv_obj_set_pos(hud_type_btn, 6, 6);
    lv_obj_set_style_radius(hud_type_btn, 8, 0);
    lv_obj_set_style_bg_color(hud_type_btn, lv_color_hex(0x0A0E17), 0);
    lv_obj_set_style_bg_opa(hud_type_btn, LV_OPA_80, 0);
    lv_obj_set_style_border_color(hud_type_btn, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_border_width(hud_type_btn, 1, 0);
    lv_obj_set_style_pad_all(hud_type_btn, 0, 0);
    lv_obj_add_event_cb(hud_type_btn, btn_toggle_type_cb, LV_EVENT_CLICKED, nullptr);

    hud_lbl_type = lv_label_create(hud_type_btn);
    lv_label_set_text(hud_lbl_type, "Road");
    lv_obj_set_style_text_color(hud_lbl_type, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_text_font(hud_lbl_type, UI_FONT_12, 0);
    lv_obj_center(hud_lbl_type);

    // 5. FLOATING HUD TOP: Tiêu đề vị trí & Mức Zoom (Giữa)
    hud_city_pill = lv_obj_create(parent);
    lv_obj_set_size(hud_city_pill, 176, 24);
    lv_obj_set_pos(hud_city_pill, 56, 6);
    lv_obj_set_style_radius(hud_city_pill, 8, 0);
    lv_obj_set_style_bg_color(hud_city_pill, lv_color_hex(0x0A0E17), 0);
    lv_obj_set_style_bg_opa(hud_city_pill, LV_OPA_80, 0);
    lv_obj_set_style_border_color(hud_city_pill, lv_color_hex(0x2D3748), 0);
    lv_obj_set_style_border_width(hud_city_pill, 1, 0);
    lv_obj_set_style_pad_all(hud_city_pill, 0, 0);
    lv_obj_clear_flag(hud_city_pill, LV_OBJ_FLAG_SCROLLABLE);

    hud_lbl_city = lv_label_create(hud_city_pill);
    lv_label_set_text_fmt(hud_lbl_city, "%s • Z%d", PRESETS[cur_preset_idx].name, cur_zoom);
    lv_obj_set_style_text_color(hud_lbl_city, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(hud_lbl_city, UI_FONT_12, 0);
    lv_obj_center(hud_lbl_city);

    // Nút chuyển địa điểm tiếp theo (Phải)
    lv_obj_t *btn_next_city = lv_btn_create(parent);
    lv_obj_set_size(btn_next_city, 34, 24);
    lv_obj_set_ext_click_area(btn_next_city, 6);
    lv_obj_set_pos(btn_next_city, 236, 6);
    lv_obj_set_style_radius(btn_next_city, 8, 0);
    lv_obj_set_style_bg_color(btn_next_city, lv_color_hex(0x0A0E17), 0);
    lv_obj_set_style_bg_opa(btn_next_city, LV_OPA_80, 0);
    lv_obj_set_style_border_color(btn_next_city, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_border_width(btn_next_city, 1, 0);
    lv_obj_add_event_cb(btn_next_city, btn_next_city_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *l_nc = lv_label_create(btn_next_city);
    lv_label_set_text(l_nc, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(l_nc, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_text_font(l_nc, UI_FONT_12, 0);
    lv_obj_center(l_nc);

    // 6. CỤM NÚT FLOATING ZOOM [+] VÀ [-] (Góc phải)
    lv_obj_t *btn_zin = lv_btn_create(parent);
    lv_obj_set_size(btn_zin, 32, 32);
    lv_obj_set_ext_click_area(btn_zin, 6);
    lv_obj_set_pos(btn_zin, 280, 36);
    lv_obj_set_style_radius(btn_zin, 16, 0);
    lv_obj_set_style_bg_color(btn_zin, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_bg_opa(btn_zin, LV_OPA_80, 0);
    lv_obj_set_style_border_color(btn_zin, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_border_width(btn_zin, 1, 0);
    lv_obj_add_event_cb(btn_zin, btn_zoom_in_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l_zin = lv_label_create(btn_zin);
    lv_label_set_text(l_zin, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(l_zin, lv_color_hex(0x00F2FE), 0);
    lv_obj_center(l_zin);

    lv_obj_t *btn_zout = lv_btn_create(parent);
    lv_obj_set_size(btn_zout, 32, 32);
    lv_obj_set_ext_click_area(btn_zout, 6);
    lv_obj_set_pos(btn_zout, 280, 72);
    lv_obj_set_style_radius(btn_zout, 16, 0);
    lv_obj_set_style_bg_color(btn_zout, lv_color_hex(0x161B26), 0);
    lv_obj_set_style_bg_opa(btn_zout, LV_OPA_80, 0);
    lv_obj_set_style_border_color(btn_zout, lv_color_hex(0x00F2FE), 0);
    lv_obj_set_style_border_width(btn_zout, 1, 0);
    lv_obj_add_event_cb(btn_zout, btn_zoom_out_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l_zout = lv_label_create(btn_zout);
    lv_label_set_text(l_zout, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(l_zout, lv_color_hex(0x00F2FE), 0);
    lv_obj_center(l_zout);

    // 7. CỤM NÚT ĐIỀU HƯỚNG D-PAD (PAN) GÓC DƯỚI PHẢI
    auto create_dpad_btn = [&](lv_coord_t x, lv_coord_t y, const char *sym, uintptr_t dir) {
        lv_obj_t *btn = lv_btn_create(parent);
        lv_obj_set_size(btn, 26, 22);
        lv_obj_set_ext_click_area(btn, 6);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x121824), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x2D3748), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_add_event_cb(btn, btn_pan_cb, LV_EVENT_CLICKED, (void *)dir);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, sym);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCBD5E0), 0);
        lv_obj_center(lbl);
        return btn;
    };

    create_dpad_btn(268, 112, LV_SYMBOL_UP, 1);    // Lên
    create_dpad_btn(240, 137, LV_SYMBOL_LEFT, 3);  // Trái
    create_dpad_btn(268, 162, LV_SYMBOL_DOWN, 2);  // Xuống
    create_dpad_btn(286, 137, LV_SYMBOL_RIGHT, 4); // Phải

    // 8. FLOATING HUD BOTTOM: Trạng thái nguồn dữ liệu góc dưới trái
    hud_source_pill = lv_obj_create(parent);
    lv_obj_set_size(hud_source_pill, 170, 22);
    lv_obj_set_pos(hud_source_pill, 6, 166);
    lv_obj_set_style_radius(hud_source_pill, 6, 0);
    lv_obj_set_style_bg_color(hud_source_pill, lv_color_hex(0x0A0E17), 0);
    lv_obj_set_style_bg_opa(hud_source_pill, LV_OPA_80, 0);
    lv_obj_set_style_border_color(hud_source_pill, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_border_width(hud_source_pill, 1, 0);
    lv_obj_set_style_pad_all(hud_source_pill, 0, 0);
    lv_obj_clear_flag(hud_source_pill, LV_OBJ_FLAG_SCROLLABLE);

    hud_lbl_source = lv_label_create(hud_source_pill);
    lv_label_set_text(hud_lbl_source, LV_SYMBOL_SD_CARD " Nạp SD Cache...");
    lv_obj_set_style_text_color(hud_lbl_source, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_text_font(hud_lbl_source, UI_FONT_12, 0);
    lv_obj_center(hud_lbl_source);

    // Kích hoạt nạp bản đồ ban đầu (Hà Nội, Z15, Roadmap)
    trigger_map_reload();
}

void map_app_close(void)
{
    app_container    = nullptr;
    map_canvas       = nullptr;
    hud_city_pill    = nullptr;
    hud_lbl_city     = nullptr;
    hud_source_pill  = nullptr;
    hud_lbl_source   = nullptr;
    hud_type_btn     = nullptr;
    hud_lbl_type     = nullptr;
    hud_coord_lbl    = nullptr;
}
