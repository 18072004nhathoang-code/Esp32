# ESP32-S3 Touch Display Multi-Board Mini OS Pro Max
**Kiến trúc:** Hardware Abstraction Layer (HAL) • FreeRTOS Multi-tasking • LVGL 8.3.11 • LovyanGFX 1.1.16 DMA • ESP32-audioI2S 3.0.12 • Dual Storage (SDMMC & SPI) • XiaoZhi AI Voice

[![Build Status](https://github.com/18072004nhathoang-code/Esp32/actions/workflows/build.yml/badge.svg)](https://github.com/18072004nhathoang-code/Esp32/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-blue)](https://www.espressif.com/)

---

## 🚀 1. Tổng quan hệ thống & Kiến trúc Đa Bo Mạch (Multi-Board HAL)

Dự án firmware Mini OS Pro Max hỗ trợ kiến trúc phân tầng phần cứng thống nhất (**Hardware Abstraction Layer - HAL**), cho phép chạy trên nhiều dòng bo mạch ESP32-S3 màn hình cảm ứng khác nhau chỉ bằng cách lựa chọn cấu hình môi trường biên dịch trong `platformio.ini`:

1. **ES3C28P 2.8" IPS HMI (Mặc định)**:
   - Bo mạch thông minh chuyên dụng trợ lý ảo AI (Xiaozhi/ChatGPT, Cheap Black Display).
   - Màn hình 2.8 inch IPS 240x320 (Xoay ngang 320x240) IC điều khiển ILI9341V.
   - Cảm ứng điện dung đa điểm FocalTech FT6336G (I2C `0x38`).
   - Thẻ nhớ MicroSD kết nối qua **SDMMC / SDIO chuyên dụng** (không chia sẻ bus với màn hình).
   - Âm thanh Codec ES8311 + IC khuếch đại PA FM8002E (Active LOW) + Micro MEMS tích hợp.
   - Không có cổng camera DVP vật lý (`BOARD_HAS_LOCAL_CAMERA 0`) -> Tự động chuyển toàn diện sang Network IP Camera (Hikvision, KBVision, Ezviz, Yoosee, ONVIF).

2. **DIYMORE ESP32-S3 3.5" IPS (Legacy Profile)**:
   - Màn hình 3.5 inch IPS 480x320 ST7796.
   - Cảm ứng điện dung FT6336U.
   - Thẻ nhớ MicroSD kết nối qua SPI Bus dùng chung (FSPI) được bảo vệ bằng `spi_bus_guard`.

```text
+-------------------------------------------------------------------------------+
|                    Mini OS Pro Max Desktop & Application Layer                 |
| [Status Bar] [System Monitor] [Google Maps Pro] [Music Player] [XiaoZhi Demo] |
|          [WiFi Hub] [Settings Control Center] [Sensors] [Camera IP/RTSP]      |
+-------------------------------------------------------------------------------+
|                     LVGL 8.3.11 High-Level Graphics Engine                    |
|       (Responsive UI 320x240 & 480x320, Montserrat Fonts, Smooth Animations)  |
+-------------------------------------------------------------------------------+
|                 FreeRTOS Multi-Tasking & Thread-Safe Porting                  |
|  - Core 1: LVGL GUI Engine (Priority 4, 12KB Stack, Mutex Protected)          |
|  - Core 0: I2S Audio Engine / MP3 Decoder (Priority 3, Dynamic Arbiter)       |
|  - Core 0: WiFi Service & Auto-Reconnect (Priority 2, Exp-Backoff 2s-60s)    |
|  - Core 0: Storage Manager (Unified SDMMC / SPI SD Hardware Abstraction)      |
+-------------------------------------------------------------------------------+
|              Hardware Abstraction Layer (include/board_config.h)               |
|  - board_es3c28p.hpp       : ILI9341V (320x240) + FT6336G + SDMMC + ES8311     |
|  - board_diymore_s3_35.hpp : ST7796 (480x320)   + FT6336U + SPI SD + ES8311    |
+-------------------------------------------------------------------------------+
|       Hardware: ESP32-S3-WROOM-1 N16R8 (Dual-Core LX7 @ 240MHz, 16M/8M OPI)   |
+-------------------------------------------------------------------------------+
```

---

## ⚡ 2. Bảng đối chiếu sơ đồ chân phần cứng (Pinout Mapping)

### A. Bo mạch ES3C28P 2.8" IPS HMI (Target mặc định)

| Module / Ngoại vi | Chức năng tín hiệu | Chân ESP32-S3 (GPIO) | Trạng thái kiểm nghiệm & Ghi chú |
| :--- | :--- | :--- | :--- |
| **Màn hình LCD (ILI9341V SPI)** | LCD_MOSI | **GPIO 11** | `[VERIFIED]` SPI2_HOST / FSPI 40MHz DMA |
| | LCD_MISO | **GPIO 13** | `[VERIFIED]` FSPI MISO |
| | LCD_SCLK | **GPIO 12** | `[VERIFIED]` FSPI SCK |
| | LCD_CS | **GPIO 10** | `[VERIFIED]` Chip Select LCD |
| | LCD_DC (TFT_RS) | **GPIO 46** | `[VERIFIED]` Data / Command Select (TFT_RS) |
| | LCD_RST | **-1** | `[VERIFIED]` Nối chân CHIP_PU (EN) |
| | LCD_BL | **GPIO 45** | `[VERIFIED]` Đèn nền LCD (LEDC PWM 44.1kHz) |
| **Cảm ứng điện dung (FT6336G)** | TOUCH_SDA | **GPIO 16** | `[VERIFIED]` I2C SDA (Dùng chung với ES8311) |
| | TOUCH_SCL | **GPIO 15** | `[VERIFIED]` I2C SCL (Dùng chung với ES8311) |
| | TOUCH_INT | **GPIO 17** | `[VERIFIED]` TP_INT (Low khi có chạm) |
| | TOUCH_RST | **GPIO 18** | `[VERIFIED]` TP_RST (Reset cảm ứng) |
| **Thẻ nhớ MicroSD (SDMMC Chuyên dụng)** | SD_CLK | **GPIO 38** | `[VERIFIED]` SDMMC CLK (Không chia sẻ với LCD) |
| | SD_CMD | **GPIO 40** | `[VERIFIED]` SDMMC CMD |
| | SD_D0 | **GPIO 39** | `[VERIFIED]` SDMMC DATA0 (Chế độ 1-bit & 4-bit) |
| | SD_D1 | **GPIO 41** | `[VERIFIED]` SDMMC DATA1 (Chế độ 4-bit) |
| | SD_D2 | **GPIO 48** | `[VERIFIED]` SDMMC DATA2 (Chế độ 4-bit) |
| | SD_D3 | **GPIO 47** | `[VERIFIED]` SDMMC DATA3 (Chế độ 4-bit) |
| **Âm thanh Codec & Micro MEMS** | I2S_MCK | **GPIO 4** | `[VERIFIED]` Master Clock ES8311 |
| *(ES8311 Codec + FM8002E PA)* | I2S_BCK | **GPIO 5** | `[VERIFIED]` Bit Clock I2S |
| | I2S_DIN | **GPIO 6** | `[VERIFIED]` Dữ liệu vào từ Micro MEMS |
| | I2S_WS | **GPIO 7** | `[VERIFIED]` Word Select / LRCK |
| | I2S_DOUT | **GPIO 8** | `[VERIFIED]` Dữ liệu ra Loa ngoài |
| | AUDIO_PA | **GPIO 1** | `[VERIFIED]` Power Amp Enable (Active LOW: 0 = Mở loa, 1 = Tắt loa) |
| | AUDIO_I2C_SDA | **GPIO 16** | `[VERIFIED]` Dùng chung I2C với Touch (`0x18`) |
| | AUDIO_I2C_SCL | **GPIO 15** | `[VERIFIED]` Dùng chung I2C với Touch (`0x18`) |
| **Ngoại vi khác** | BOOT KEY | **GPIO 0** | `[VERIFIED]` Nút nhấn Boot/Flash |
| | RGB LED | **GPIO 42** | `[VERIFIED]` WS2812 RGB LED đơn tuyến |
| | BATTERY ADC | **GPIO 9** | `[VERIFIED]` Đo điện áp pin |
| | UART0 TX / RX | **GPIO 43 / 44** | `[VERIFIED]` Nạp Serial / Monitor |
| | Camera DVP | *Không hỗ trợ* | `[VERIFIED]` Board không có camera vật lý (`BOARD_HAS_LOCAL_CAMERA 0`) |

---

### B. Bo mạch DIYMORE ESP32-S3 3.5" IPS (Legacy Target)

| Module / Ngoại vi | Chức năng tín hiệu | Chân ESP32-S3 (GPIO) | Trạng thái kiểm nghiệm |
| :--- | :--- | :--- | :--- |
| **Màn hình LCD (ST7796 SPI)** | MOSI, MISO, SCK, CS, DC, BL | **11, 13, 12, 10, 4, 45** | `[TESTED]` Đã test thực tế |
| **Cảm ứng (FT6336U)** | SDA, SCL, RST | **8, 9, 3** | `[TESTED]` Đã test thực tế |
| **Thẻ nhớ MicroSD (SPI)** | MOSI, MISO, SCK, CS | **11, 13, 12, 42** | `[TESTED]` Bus FSPI dùng chung với LCD |
| **Âm thanh I2S & Codec** | BCLK, WS, DOUT, DIN, MCLK, PA | **18, 21, 15, 16, 17, 1** | `[TESTED]` Đã test thực tế |
| | I2C SDA / SCL (Codec) | **38, 39** | `[TESTED]` Đã test thực tế |

> [!IMPORTANT]
> **Bảo vệ Bus SPI (FSPI)**: Màn hình ST7796 và thẻ nhớ MicroSD chia sẻ GPIO 11, 12, 13. Hệ thống sử dụng `spi_bus_lock()` và `spi_bus_unlock()` trong `spi_bus_guard.cpp` để đợi DMA màn hình (`gfx.waitDMA()`) hoàn tất trước khi thao tác thẻ SD. Nếu lock fail, frame vẽ sẽ bị bỏ qua và tuyệt đối không truy cập SPI khi chưa chiếm được bus.

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
| **WiFi_Manager** | **Core 0** | **2** | Event-driven, quản lý kết nối, hỗ trợ quên mạng (`forget_network`) và bật/tắt auto-reconnect, bảo vệ NVS bằng mutex. |
| **Map_Worker** | **Core 0** | **2** | Tải tile HTTP/HTTPS qua FreeRTOS Queue, giải mã JPEG ping-pong buffer vào PSRAM, TLS Root CA bundle. |
| **AI_Voice_Task** | **Core 0** | **2** | Demo/Mock mô phỏng tương tác giọng nói cục bộ (không rò rỉ API key khi build/log). |

---

## 🔒 5. Cấu hình Bảo mật và API Keys (Secrets)

1. Sao chép file mẫu:
   ```bash
   cp include/secrets.example.h include/secrets.h
   ```
2. Cập nhật thông tin trong `include/secrets.h`:
   ```c
   #define DEFAULT_WIFI_SSID           "Your_SSID"
   #define DEFAULT_WIFI_PASS           "Your_Password"
   #define GOOGLE_MAPS_STATIC_API_KEY  "AIzaSy..."
   #define GEMINI_API_KEY              "AIzaSy..."
   ```
3. File `include/secrets.h` đã được thêm vào `.gitignore` để bảo vệ an toàn thông tin cá nhân.

---

## 🛠️ 6. Biên dịch và Nạp Firmware

```bash
# 1. Biên dịch mục tiêu mặc định: Bo mạch ES3C28P 2.8" IPS HMI (Shopee / Xiaozhi)
pio run -e esp32-s3-es3c28p

# Nạp firmware vào bo mạch ES3C28P
pio run -e esp32-s3-es3c28p -t upload

# 2. Biên dịch mục tiêu phụ: Bo mạch DIYMORE ESP32-S3 3.5" IPS (ST7796)
pio run -e esp32-s3-mini-os

# Nạp firmware vào bo mạch DIYMORE
pio run -e esp32-s3-mini-os -t upload

# 3. Mở Serial Monitor để theo dõi hệ thống (115200 baud)
pio device monitor -b 115200
```

---

## 📱 7. Các phân hệ ứng dụng

1. **System Monitor**: Đo thời gian thực tần số CPU (240MHz), dung lượng RAM nội, 8MB Octal PSRAM, nhiệt độ lõi chip (°C) và Uptime.
2. **Google Maps Pro**: Chế độ bản đồ Vector Offline WGS84 kèm chế độ Online Google Satellite / Roadmap Tiles với cơ chế đệm kép Ping-Pong Buffer và API tiêu thụ atomic dưới mutex trong PSRAM.
3. **Music Player**: Trình phát nhạc MP3 giao diện chia đôi cột, animation đĩa than quay xoay tròn, quản lý sở hữu bus I2S độc quyền khi phát nhạc, bảo vệ I/O thẻ nhớ bằng `spi_bus_guard`.
4. **XiaoZhi AI Voice (Demo/Mock)**: Giao diện chat bong bóng hội thoại phong cách iOS/iMessage, nút Push-To-Talk, mô phỏng phản hồi giả lập nội bộ.
5. **WiFi Hub & Control Center**: Quét mạng 2.4GHz, ghi nhớ mạng với Preferences thread-safe, hỗ trợ quên mạng và tùy chọn tự động kết nối lại.
6. **Camera Subsystem**:
   - **HTTP Snapshot (JPEG)**: `READY` (Nạp và hiển thị ảnh tĩnh IP camera thực tế).
   - **ONVIF Client**: `PARTIAL / FALLBACK ADAPTER` (Hỗ trợ cấu trúc SOAP và adapter các hãng Hikvision, KBVision, Ezviz, Yoosee).
   - **MJPEG & RTSP / H.264**: `NOT_IMPLEMENTED` (Khung giao diện phân tầng scaffold chuẩn, chưa hoàn chỉnh bộ giải mã video).
   - **Local DVP (OV2640/OV5640)**: Driver abstraction layer sẵn sàng, trạng thái vật lý `NOT_DETECTED`.

---

## ⚖️ 8. Giấy phép mã nguồn
Dự án được phát hành theo giấy phép **MIT License**.
