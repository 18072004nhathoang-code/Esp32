# ESP32-S3 2.8" Touch Display Mini OS Starter Kit
**Công nghệ:** VS Code + PlatformIO + Thư viện đồ họa LVGL 8 + Driver siêu tốc LovyanGFX (DMA)

---

## 🚀 1. Tổng quan kiến trúc hệ thống

Bộ mã nguồn này được thiết kế theo chuẩn kiến trúc hệ điều hành nhúng đa luồng (Multi-tasking Embedded OS) trên vi điều khiển **ESP32-S3 Dual-Core (240MHz)**:

```
+-------------------------------------------------------------------------+
|                  Mini OS Application Layer (Desktop & Apps)             |
|    [Status Bar]   [System Monitor]   [Google Maps]   [Settings]  [WiFi] |
+-------------------------------------------------------------------------+
|                  LVGL 8.3.x High-Level Graphics Engine                  |
|          (Canvas, Widgets, Animations, Themes, Font Montserrat)         |
+-------------------------------------------------------------------------+
|               FreeRTOS Porting Layer (Thread-Safe Mutex)                |
|   - Core 1: Dedicated LVGL Task (lv_timer_handler @ 60 FPS)             |
|   - Core 0: Background Services (Sensors, WiFi, Logic, File I/O)        |
|   - Double DMA Framebuffers (SRAM nội tối ưu hóa băng thông)            |
+-------------------------------------------------------------------------+
|                  LovyanGFX Hardware Acceleration Driver                 |
|           (SPI Hardware DMA 40MHz - 80MHz, Hardware PWM Light)          |
+-------------------------------------------------------------------------+
|                   ESP32-S3 2.8" Hardware (TFT + Touch)                  |
+-------------------------------------------------------------------------+
```

---

## 📁 2. Cấu trúc thư mục dự án

```
d:/Esp32/
├── platformio.ini              # Cấu hình nạp, xung nhịp, PSRAM OPI, cờ build LVGL
├── partitions.csv              # Bảng phân vùng bộ nhớ (Hỗ trợ 16MB & 8MB Flash)
├── include/
│   ├── lv_conf.h               # Cấu hình chuẩn LVGL 8.3 (Depth 16, Fonts, Widgets)
│   └── LGFX_Config.hpp         # Driver phần cứng LovyanGFX (Định nghĩa chân LCD/Touch)
├── src/
│   ├── apps/
│   │   ├── map_app.h           # Header ứng dụng Google Maps
│   │   └── map_app.cpp         # Triển khai Dual-Engine: Offline Vector Canvas & Online Tile
│   ├── display/
│   │   ├── lvgl_port.h         # API khóa Mutex, khởi tạo DMA & FreeRTOS Task
│   │   └── lvgl_port.cpp       # Triển khai Flush DMA và Touchpad Reader
│   ├── os/
│   │   ├── system_info.h       # Cấu trúc dữ liệu đo RAM, PSRAM, CPU, Nhiệt độ chip
│   │   └── system_info.cpp     # Thu thập dữ liệu phần cứng theo thời gian thực
│   ├── ui/
│   │   ├── ui_manager.h        # Khởi tạo giao diện Desktop & Status bar
│   │   └── ui_manager.cpp      # Desktop vuốt cuộn mượt mà, định tuyến các App
│   └── main.cpp                # Điểm khởi động hệ thống
└── README.md                   # Tài liệu hướng dẫn chi tiết
```

---

## 🗺️ 3. Ứng dụng xem bản đồ Google Maps

Ứng dụng bản đồ được tối ưu riêng cho màn hình cảm ứng 2.8" (320x240) với kiến trúc **Dual-Engine**:

1. **Chế độ Ngoại tuyến (Offline Vector Engine - Mặc định):**
   - Không cần kết nối WiFi hay cấu hình API Key, mở lên chạy ngay lập tức.
   - Sử dụng bộ đệm Canvas 230x155 px được cấp phát trực tiếp từ vùng nhớ **PSRAM 8MB**.
   - Vẽ mô phỏng bản đồ phong cách **Google Maps Dark Theme**: sông hồ, cao tốc màu cam (#F88F24), đường nội đô, công viên xanh, la bàn Bắc và **tâm ghim vị trí màu đỏ Google (#EA4335)**.
2. **Bảng điều khiển cảm ứng bên phải (Toolbar):**
   - **`[ + ]` / `[ - ]`**: Phóng to, thu nhỏ bản đồ (Mức zoom từ 5 đến 19).
   - **`[ ▲ ▼ ◄ ► ]`**: Cụm phím D-Pad dịch chuyển tọa độ bản đồ theo 4 hướng.
   - **`[ 📍 City ]`**: Chuyển nhanh giữa các tọa độ nổi tiếng:
     - Hà Nội (Hồ Gươm: `21.0285° N, 105.8542° E`)
     - TP. Hồ Chí Minh (Chợ Bến Thành: `10.7725° N, 106.6980° E`)
     - Đà Nẵng (Cầu Rồng: `16.0611° N, 108.2274° E`)
     - Tokyo (Ngã tư Shibuya: `35.6595° N, 139.7005° E`)
     - Paris (Tháp Eiffel: `48.8584° N, 2.2945° E`)
     - New York (Times Square: `40.7580° N, -73.9855° W`)
   - **`[ 🌐 Net ]`**: Bật/tắt chế độ trực tuyến qua WiFi (Google Static Maps hoặc OpenStreetMap).
3. **Thanh thông tin đáy (Footer):**
   - Hiển thị tên địa điểm, tọa độ WGS84 chính xác (Lat, Lon), mức Zoom hiện tại và trạng thái kết nối mạng.

---

## ⚡ 4. Sơ đồ chân phần cứng (Pinout Mapping)

Trong file [`include/LGFX_Config.hpp`](include/LGFX_Config.hpp), bạn có thể chuyển đổi giữa các bo mạch chỉ bằng việc chọn `#define`:

### A. Sunton ESP32-S3-2432S028R (Cảm ứng điện trở XPT2046 - MẶC ĐỊNH)
* **Màn hình LCD (ST7789 / ILI9341 - SPI2_HOST):**
  * `MOSI`: GPIO 11
  * `MISO`: GPIO 13
  * `SCK`:  GPIO 12
  * `DC`:   GPIO 4
  * `CS`:   GPIO 10
  * `RST`:  -1 (EN)
  * `BL`:   GPIO 16 (PWM điều khiển độ sáng)
* **Cảm ứng (XPT2046):**
  * `TOUCH_MOSI`: GPIO 11 (Dùng chung bus SPI với LCD)
  * `TOUCH_MISO`: GPIO 13
  * `TOUCH_SCK`:  GPIO 12
  * `TOUCH_CS`:   GPIO 33
  * `TOUCH_IRQ`:  GPIO 36

### B. Sunton ESP32-S3-2432S028C (Cảm ứng điện dung CST816S / GT911)
* **LCD:** Giống bản R ở trên.
* **Cảm ứng I2C:**
  * `SDA`: GPIO 4 (hoặc 19)
  * `SCL`: GPIO 5 (hoặc 20)
  * `INT`: GPIO 0 (hoặc 18)
  * `RST`: GPIO 1 (hoặc 38)

*(Chỉ cần mở comment `#define BOARD_SUNTON_S3_28C` trong `LGFX_Config.hpp`)*

---

## 🛠️ 5. Hướng dẫn sử dụng với VS Code + PlatformIO

1. **Mở dự án:**
   - Trong VS Code: Chọn `File` -> `Open Folder...` -> Chọn thư mục `d:\Esp32`.
   - PlatformIO IDE sẽ tự động nhận diện `platformio.ini` và cấu hình môi trường.
2. **Biên dịch & Nạp:**
   - Cắm cáp USB Type-C kết nối ESP32-S3 với máy tính.
   - Nhấn **Build** (`✓`) rồi nhấn **Upload** (`→`).
   - Mở **Serial Monitor** (115200 baud) để theo dõi nhật ký khởi động hệ thống.

---

## 🧠 6. Nguyên lý an toàn luồng (Thread-Safety) khi viết App

```cpp
#include "display/lvgl_port.h"

// Khi muốn cập nhật giao diện từ một Task nền bất kỳ:
if (lvgl_port_lock(100)) // Đợi tối đa 100ms
{
    lv_label_set_text(my_label, "Dữ liệu mới");
    lvgl_port_unlock(); // BẮT BUỘC mở khóa sau khi xong
}
```
