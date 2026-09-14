# DIYMORE ESP32-S3 3.5" IPS Touch Display Mini OS Pro Max
**Kiến trúc:** FreeRTOS Multi-tasking Embedded OS • LVGL 8.3.11 • LovyanGFX 1.1.16 DMA • ESP32-audioI2S 3.0.12 • XiaoZhi AI Voice (Demo/Mock)

[![Build Status](https://github.com/18072004nhathoang-code/Esp32/actions/workflows/build.yml/badge.svg)](https://github.com/18072004nhathoang-code/Esp32/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-blue)](https://www.espressif.com/)

---

## 🚀 1. Tổng quan hệ thống

Dự án firmware Mini OS Pro Max biến bo mạch **ESP32-S3 N16R8 (16MB Flash Quad/QIO + 8MB OPI PSRAM)** thành thiết bị cầm tay thông minh đa nhiệm:
- Giao diện cảm ứng điện dung mượt mà 60 FPS tăng tốc phần cứng DMA qua LovyanGFX.
- Kiến trúc Bus Guard dùng chung mutex FreeRTOS cho bus SPI giữa màn hình ST7796 và thẻ nhớ MicroSD.
- Hệ thống Audio Arbitration động quản lý quyền sở hữu `I2S_NUM_0` giữa `audio_manager` (Tone, Recorder, Sound FX) và `ESP32-audioI2S` (Music Player MP3).
- Bản đồ trực tuyến Google Maps (Roadmap & Satellite) và ngoại tuyến Vector WGS84 với bộ đệm Double-Buffering PSRAM loại bỏ xé hình.
- Phân hệ Camera hợp nhất: DVP cục bộ (OV2640/OV5640) và Network Stream (RTSP/ONVIF/HTTP Snapshot cho Hikvision, KBVision, Ezviz, Yoosee).
- Trợ lý giọng nói ảo XiaoZhi AI Voice (mô phỏng Demo/Mock trên Core 0).

```text
+-------------------------------------------------------------------------------+
|                    Mini OS Pro Max Desktop & Application Layer                 |
| [Status Bar] [System Monitor] [Google Maps Pro] [Music Player] [XiaoZhi Demo] |
|          [WiFi Hub] [Settings Control Center] [Sensors] [Camera DVP/RTSP]     |
+-------------------------------------------------------------------------------+
|                     LVGL 8.3.11 High-Level Graphics Engine                    |
|      (Glassmorphism UI, Responsive Widgets, Montserrat Fonts, Animations)     |
+-------------------------------------------------------------------------------+
|                 FreeRTOS Multi-Tasking & Thread-Safe Porting                  |
|  - Core 1: LVGL GUI Engine (Priority 4, 16KB Stack, Mutex Protected)          |
|  - Core 0: I2S Audio Engine / MP3 Decoder (Priority 3, Dynamic Arbiter)       |
|  - Core 0: WiFi Service & Auto-Reconnect (Priority 2, Exp-Backoff 2s-60s)    |
|  - Core 0: Map Tile Downloader Queue Worker (Priority 2, Ping-Pong Buffers)   |
|  - Core 0: XiaoZhi AI Voice Streaming (Priority 2, Local Demo Task)           |
+-------------------------------------------------------------------------------+
|                  Hardware Abstraction & Hardware DMA Acceleration             |
|  - LovyanGFX SPI DMA 40MHz (ST7796 IPS LCD, Hardware WaitDMA Synchronization) |
|  - spi_bus_guard: FreeRTOS Mutex chia sẻ an toàn bus FSPI giữa LCD & MicroSD  |
|  - I2S Ownership Arbiter: Cấp phát và giải phóng I2S_NUM_0 theo nhu cầu       |
|  - Hardware PWM Backlight Timer & Power Management Display Sleep Mode         |
+-------------------------------------------------------------------------------+
|       Hardware: ESP32-S3-WROOM-1 N16R8 (Dual-Core LX7 @ 240MHz, 16M/8M OPI)   |
+-------------------------------------------------------------------------------+
```

---

## ⚡ 2. Sơ đồ đấu nối chân phần cứng (Pinout Mapping)

### A. Bo mạch thực tế: DIYMORE ESP32-S3 3.5" IPS (ST7796 480x320 Capacitive Touch)

| Module / Ngoại vi | Chức năng tín hiệu | Chân ESP32-S3 (GPIO) | Trạng thái xác nhận & Ghi chú |
| :--- | :--- | :--- | :--- |
| **Màn hình LCD (ST7796 SPI)** | LCD_MOSI | **GPIO 11** | Đã xác nhận (SPI2_HOST / FSPI, 40MHz DMA) |
| | LCD_MISO | **GPIO 13** | Đã xác nhận (Dùng chung bus SPI với MicroSD) |
| | LCD_SCLK | **GPIO 12** | Đã xác nhận (Xung nhịp SPI chung) |
| | LCD_CS | **GPIO 10** | Đã xác nhận (Chip Select LCD) |
| | LCD_DC | **GPIO 4** | Đã xác nhận (Data / Command Select) |
| | LCD_RST | **-1** | Nối qua chân RESET chung hoặc EN |
| | LCD_BL | **GPIO 45** | Đã xác nhận (Điều khiển độ sáng LEDC PWM 1.2kHz) |
| **Cảm ứng điện dung** | TOUCH_SDA | **GPIO 8** | Đã xác nhận (`LGFX_Config.hpp`) |
| *(FT6336U / FT5x06 / GT911 / CST816S)* | TOUCH_SCL | **GPIO 9** | Đã xác nhận (`LGFX_Config.hpp`) |
| | TOUCH_INT | **-1** | Polling I2C định kỳ từ LVGL task |
| | TOUCH_RST | **GPIO 3** | Đã xác nhận (`LGFX_Config.hpp`) |
| **Thẻ nhớ MicroSD (SPI Slot)** | SD_MOSI | **GPIO 11** | Đã xác nhận (Bảo vệ qua `spi_bus_guard`) |
| | SD_MISO | **GPIO 13** | Đã xác nhận (Bảo vệ qua `spi_bus_guard`) |
| | SD_SCLK | **GPIO 12** | Đã xác nhận (Bảo vệ qua `spi_bus_guard`) |
| | SD_CS | **GPIO 42** | Đã xác nhận (Chip Select riêng cho thẻ MicroSD) |
| **Âm thanh I2S Duplex** | I2S_BCK | **GPIO 18** | Đã xác nhận (`audio_manager.h`) |
| *(MAX98357A / ES8388 / INMP441)* | I2S_WS | **GPIO 21** | Đã xác nhận (`audio_manager.h`) |
| | I2S_DOUT | **GPIO 15** | Đã xác nhận (Dữ liệu ra Loa) |
| | I2S_DIN | **GPIO 16** | Đã xác nhận (Dữ liệu vào từ Micro MEMS) |
| | I2S_MCK | **GPIO 17** | Đã xác nhận (Master Clock) |
| | I2S_PA_EN | **GPIO 1** | Đã xác nhận (Kích hoạt công suất Audio Amp) |
| **Camera DVP 8-bit cục bộ** | D0 - D7, XCLK, VSYNC... | *Header dự phòng* | ⚠️ **Chưa xác nhận thực tế** (Chờ sơ đồ schematic phần cứng chi tiết của bo mạch) |
| **Biến thể Cảm ứng GT911 / CST816** | Chân INT / Địa chỉ I2C | *0x5D / 0x15* | ⚠️ **Chưa xác nhận thực tế** (Hỗ trợ cấu hình qua macro `TOUCH_CONTROLLER_*`) |

> [!IMPORTANT]
> **Bảo vệ Bus SPI (FSPI)**: Màn hình ST7796 và thẻ nhớ MicroSD chia sẻ GPIO 11, 12, 13. Hệ thống sử dụng `spi_bus_lock()` và `spi_bus_unlock()` trong `spi_bus_guard.cpp` để đợi DMA màn hình (`gfx.waitDMA()`) hoàn tất trước khi thao tác thẻ SD, loại bỏ hoàn toàn lỗi xung đột bus phần cứng.

---

## 📦 3. Thư viện phụ thuộc (Pinned Dependencies)

Tất cả các thư viện trong `platformio.ini` được khóa phiên bản chính xác để đảm bảo tính tái lặp và ổn định tuyệt đối của bản build:

| Thư viện | Phiên bản cố định | Mục đích sử dụng |
| :--- | :--- | :--- |
| `lvgl/lvgl` | **8.3.11** | Nhân giao diện đồ họa chính |
| `lovyan03/LovyanGFX` | **1.1.16** | Driver đồ họa ST7796 SPI & Touch Controller |
| `madhephaestus/ESP32Encoder` | **0.11.7** | Đọc rotary encoder nếu có ngoại vi |
| `ESP32-audioI2S` | **3.0.12** (Git commit `#3.0.12`) | Giải mã MP3 từ thẻ nhớ SD qua I2S |
| `bodmer/TJpg_Decoder` | **1.1.0** | Giải mã ảnh JPEG Google Maps vào PSRAM |

---

## 🧵 4. Phân bổ tài nguyên FreeRTOS (Core & Priority)

| Tên Luồng / Task | Nhân Core | Priority | Cơ chế vận hành & Vai trò |
| :--- | :--- | :--- | :--- |
| **LVGL_Task** | **Core 1** | **4** | Chu kỳ 10ms, cập nhật UI, xử lý chạm cảm ứng, đồng bộ qua `lvgl_port_lock()` và `spi_bus_lock()`. |
| **AudioTask (MP3)** | **Core 0** | **3** | Chu kỳ 2ms, giải mã âm thanh từ SD, tự động uninstall/reinstall driver I2S theo nhu cầu. |
| **WiFi_Manager** | **Core 0** | **2** | Event-driven, quản lý kết nối, hỗ trợ quên mạng (`forget_network`) và bật/tắt auto-reconnect. |
| **Map_Worker** | **Core 0** | **2** | Tải tile HTTP/HTTPS qua FreeRTOS Queue, giải mã JPEG ping-pong buffer vào PSRAM. |
| **AI_Voice_Task** | **Core 0** | **2** | Demo/Mock mô phỏng tương tác giọng nói cục bộ (không rò rỉ API key khi build/log). |

---

## 🔒 5. Cấu hình Bảo mật và API Keys (Secrets)

1. Sao chép file mẫu:
   ```bash
   cp include/secrets.example.h include/secrets.h
   ```
2. Cập nhật thông tin trong `include/secrets.h`:
   ```c
   #define WIFI_DEFAULT_SSID       "Your_SSID"
   #define WIFI_DEFAULT_PASS       "Your_Password"
   #define GOOGLE_MAPS_API_KEY     "AIzaSy..."
   #define GEMINI_API_KEY          "AIzaSy..."
   ```
3. File `include/secrets.h` đã được thêm vào `.gitignore` để bảo vệ an toàn thông tin cá nhân.

---

## 🛠️ 6. Biên dịch và Nạp Firmware

```bash
# Biên dịch mã nguồn với PlatformIO
pio run

# Nạp firmware vào bo mạch ESP32-S3
pio run -t upload

# Mở Serial Monitor để theo dõi hệ thống (115200 baud)
pio run -t monitor
```

---

## 📱 7. Các phân hệ ứng dụng

1. **System Monitor**: Đo thời gian thực tần số CPU (240MHz), dung lượng RAM nội, 8MB Octal PSRAM, nhiệt độ lõi chip (°C) và Uptime.
2. **Google Maps Pro**: Chế độ bản đồ Vector Offline WGS84 kèm chế độ Online Google Satellite / Roadmap Tiles với cơ chế đệm kép Ping-Pong Buffer trong PSRAM.
3. **Music Player**: Trình phát nhạc MP3 giao diện chia đôi cột, animation đĩa than quay xoay tròn, quản lý sở hữu bus I2S độc quyền khi phát nhạc.
4. **XiaoZhi AI Voice (Demo/Mock)**: Giao diện chat bong bóng hội thoại phong cách iOS/iMessage, nút Push-To-Talk, mô phỏng phản hồi giả lập nội bộ.
5. **WiFi Hub & Control Center**: Quét mạng 2.4GHz, ghi nhớ mạng, hỗ trợ quên mạng và tùy chọn tự động kết nối lại.
6. **Camera Subsystem**: Tầng kiến trúc tách biệt giữa `LocalCameraService` (DVP OV2640/OV5640) và `NetworkCameraService` (ONVIF/RTSP/HTTP snapshot cho Hikvision, KBVision, Ezviz, Yoosee).

---

## ⚖️ 8. Giấy phép mã nguồn
Dự án được phát hành theo giấy phép **MIT License**.
