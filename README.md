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
   - Màn hình 2.8 inch IPS 240x320 Portrait chuẩn Mobile OS (Orientation: Portrait 240x320) IC điều khiển ILI9341V.
   - Cảm ứng điện dung đa điểm FocalTech FT6336G (I2C `0x38`).
   - Thẻ nhớ MicroSD kết nối qua **SDMMC / SDIO chuyên dụng** (không chia sẻ bus với màn hình).
   - Âm thanh Codec ES8311 + IC khuếch đại PA FM8002E (Active LOW) + Micro MEMS tích hợp.
   - Không có cổng camera DVP vật lý (`BOARD_HAS_LOCAL_CAMERA 0`) -> Tự động chuyển toàn diện sang Network IP Camera (Hikvision, KBVision, Ezviz, Yoosee, ONVIF).

2. **DIYMORE ESP32-S3 3.5" IPS (Legacy Profile)**:
   - Màn hình 3.5 inch IPS ST7796.
   - Cảm ứng điện dung FT6336U.
   - Thẻ nhớ MicroSD kết nối qua SPI Bus dùng chung (FSPI) được bảo vệ bằng `spi_bus_guard`.

```text
+-------------------------------------------------------------------------------+
|                    Mini OS Desktop & Application Layer (240x320)              |
| [Status Bar] [System Monitor] [Google Maps] [Music Player] [XiaoZhi AI Voice] |
|          [WiFi Settings] [Control Center] [Power Manager] [Camera IP]         |
+-------------------------------------------------------------------------------+
|                     LVGL 8.3.11 High-Level Graphics Engine                    |
|          (Modern TikTok / Mobile OS Style, 240x320 Portrait, Fast 60 FPS)     |
+-------------------------------------------------------------------------------+
|                 FreeRTOS Multi-Tasking & Thread-Safe Porting                  |
|  - Core 1: LVGL GUI Engine (Priority 4, 12KB Stack, Mutex Protected)          |
|  - Core 0: I2S Audio Engine / MP3 Decoder (Priority 3, Dynamic Arbiter)       |
|  - Core 0: WiFi Service & Auto-Reconnect (Priority 2, Exp-Backoff 2s-60s)    |
|  - Core 0: Storage Manager (Unified SDMMC / SPI SD Hardware Abstraction)      |
+-------------------------------------------------------------------------------+
|              Hardware Abstraction Layer (include/board_config.h)               |
|  - board_es3c28p.hpp       : ILI9341V (240x320 Portrait) + FT6336G + SDMMC   |
|  - board_diymore_s3_35.hpp : ST7796 (Portrait HAL)        + FT6336U + SPI SD   |
+-------------------------------------------------------------------------------+
|       Hardware: ESP32-S3-WROOM-1 N16R8 (Dual-Core LX7 @ 240MHz, 16M/8M OPI)   |
+-------------------------------------------------------------------------------+
```

---

## ⚡ 2. Bảng đối chiếu sơ đồ chân phần cứng (Pinout Mapping)

### A. Bo mạch ES3C28P 2.8" IPS HMI (Mục tiêu phần cứng mới)

| Module / Ngoại vi | Chức năng tín hiệu | Chân ESP32-S3 (GPIO) | Trạng thái phần cứng & Ghi chú |
| :--- | :--- | :--- | :--- |
| **Màn hình LCD (ILI9341V SPI)** | LCD_MOSI | **GPIO 11** | `Configured in firmware` (SPI2_HOST / FSPI 40MHz DMA) |
| | LCD_MISO | **GPIO 13** | `Configured in firmware` (FSPI MISO) |
| | LCD_SCLK | **GPIO 12** | `Configured in firmware` (FSPI SCK) |
| | LCD_CS | **GPIO 10** | `Configured in firmware` (Chip Select LCD) |
| | LCD_DC (TFT_RS) | **GPIO 46** | `Configured in firmware` (Data / Command Select) |
| | LCD_RST | **-1** | `Configured in firmware` (Mạch RC Reset phần cứng) |
| | LCD_BL | **GPIO 45** | `Configured in firmware` (Đèn nền LEDC PWM 44.1kHz) |
| **Cảm ứng điện dung (FT6336G)** | TOUCH_SDA | **GPIO 16** | `Configured in firmware` (I2C SDA dùng chung Codec) |
| | TOUCH_SCL | **GPIO 15** | `Configured in firmware` (I2C SCL dùng chung Codec) |
| | TOUCH_INT | **GPIO 17** | `Configured in firmware` (Ngắt cảm ứng TP_INT) |
| | TOUCH_RST | **GPIO 18** | `Configured in firmware` (Reset cảm ứng TP_RST) |
| **Thẻ nhớ MicroSD (SDMMC Chuyên dụng)** | SD_CLK | **GPIO 38** | `Configured in firmware` (SDMMC CLK độc lập với LCD) |
| | SD_CMD | **GPIO 40** | `Configured in firmware` (SDMMC CMD) |
| | SD_D0 | **GPIO 39** | `Configured in firmware` (SDMMC DATA0) |
| | SD_D1 | **GPIO 41** | `Configured in firmware` (SDMMC DATA1 4-bit) |
| | SD_D2 | **GPIO 48** | `Configured in firmware` (SDMMC DATA2 4-bit) |
| | SD_D3 | **GPIO 47** | `Configured in firmware` (SDMMC DATA3 4-bit) |
| **Âm thanh Codec & Micro MEMS** | I2S_MCK | **GPIO 4** | `Configured in firmware` (Master Clock ES8311) |
| *(ES8311 Codec + FM8002E PA)* | I2S_BCK | **GPIO 5** | `Configured in firmware` (Bit Clock I2S) |
| | I2S_DIN | **GPIO 6** | `Configured in firmware` (Dữ liệu vào từ Micro MEMS) |
| | I2S_WS | **GPIO 7** | `Configured in firmware` (Word Select / LRCK) |
| | I2S_DOUT | **GPIO 8** | `Configured in firmware` (Dữ liệu ra Loa ngoài) |
| | AUDIO_PA | **GPIO 1** | `Configured in firmware` (Power Amp Enable: Active LOW) |
| | AUDIO_I2C_SDA | **GPIO 16** | `Configured in firmware` (Địa chỉ Codec `0x18`) |
| | AUDIO_I2C_SCL | **GPIO 15** | `Configured in firmware` (Địa chỉ Codec `0x18`) |
| **Ngoại vi khác** | BOOT KEY | **GPIO 0** | `Configured in firmware` (Nút nhấn Boot) |
| | RGB LED | **GPIO 42** | `Configured in firmware` (Status LED) |
| | BATTERY ADC | **GPIO 9** | `Configured in firmware` (Đo điện áp pin) |
| | UART0 TX / RX | **GPIO 43 / 44** | `Configured in firmware` (Nạp Serial / Monitor) |
| | Camera DVP | *Không hỗ trợ* | `Configured in firmware` (`BOARD_HAS_LOCAL_CAMERA 0`) |

---

### B. Bo mạch DIYMORE ESP32-S3 3.5" IPS (Legacy Target)

| Module / Ngoại vi | Chức năng tín hiệu | Chân ESP32-S3 (GPIO) | Trạng thái kiểm nghiệm |
| :--- | :--- | :--- | :--- |
| **Màn hình LCD (ST7796 SPI)** | MOSI, MISO, SCK, CS, DC, BL | **11, 13, 12, 10, 4, 45** | `[TESTED]` Đã test thực tế trên phần cứng cũ |
| **Cảm ứng (FT6336U)** | SDA, SCL, RST | **8, 9, 3** | `[TESTED]` Đã test thực tế trên phần cứng cũ |
| **Thẻ nhớ MicroSD (SPI)** | MOSI, MISO, SCK, CS | **11, 13, 12, 42** | `[TESTED]` Bus FSPI dùng chung với LCD |
| **Âm thanh I2S & Codec** | BCLK, WS, DOUT, DIN, MCLK, PA | **18, 21, 15, 16, 17, 1** | `[TESTED]` Đã test thực tế trên phần cứng cũ |
| | I2C SDA / SCL (Codec) | **38, 39** | `[TESTED]` Đã test thực tế trên phần cứng cũ |

> [!IMPORTANT]
> **Shared I2C Bus (ES3C28P)**: Màn hình cảm ứng FT6336G (`0x38`) và Audio Codec ES8311 (`0x18`) chia sẻ cùng chân GPIO 16 (SDA) và GPIO 15 (SCL). Hệ thống sử dụng module `shared_i2c_bus` với duy nhất một Wire controller được đồng bộ bằng FreeRTOS Mutex (`shared_i2c_lock` / `shared_i2c_unlock`), ngăn chặn xung đột driver hoặc tranh chấp bus.
>
> **Bảo vệ Bus SPI (FSPI trên DIYMORE)**: Màn hình ST7796 và thẻ nhớ MicroSD chia sẻ GPIO 11, 12, 13. Hệ thống sử dụng `spi_bus_lock()` và `spi_bus_unlock()` trong `spi_bus_guard.cpp` để đợi DMA màn hình (`gfx.waitDMA()`) hoàn tất trước khi thao tác thẻ SD. Nếu lock fail, frame vẽ sẽ bị bỏ qua và tuyệt đối không truy cập SPI khi chưa chiếm được bus.

---

## 📦 3. Thư viện phụ thuộc (Pinned Dependencies)

Tất cả các thư viện trong `platformio.ini` được khóa phiên bản chính xác để đảm bảo tính tái lặp và ổn định tuyệt đối của bản build:

| Thư viện | Phiên bản cố định | Mục đích sử dụng |
| :--- | :--- | :--- |
| `lvgl/lvgl` | **8.3.11** | Nhân giao diện đồ họa chính |
| `lovyan03/LovyanGFX` | **1.1.16** | Driver đồ họa ILI9341V / ST7796 SPI & Touch Controller |
| `madhephaestus/ESP32Encoder` | **0.11.7** | Đọc rotary encoder nếu có ngoại vi |
| `ESP32-audioI2S` | **3.0.12** (Git commit `#3.0.12`) | Giải mã MP3 từ thẻ nhớ SD qua I2S |
| `bodmer/TJpg_Decoder` | **1.1.0** | Giải mã ảnh JPEG Google Maps & Camera Snapshot vào PSRAM |

---

## 🧵 4. Phân bổ tài nguyên FreeRTOS (Core & Priority)

| Tên Luồng / Task | Nhân Core | Priority | Cơ chế vận hành & Vai trò |
| :--- | :--- | :--- | :--- |
| **LVGL_Task** | **Core 1** | **4** | Chu kỳ 10ms, cập nhật UI, xử lý chạm cảm ứng, đồng bộ qua `lvgl_port_lock()` và `spi_bus_lock()`. |
| **MusicAudioTask (MP3)** | **Core 0** | **3** | Nhận lệnh qua FreeRTOS Queue, giải mã MP3, đồng bộ `audio_mutex` và `storage_lock` an toàn tuyệt đối không deadlock. |
| **Audio_Task (AudioManager)** | **Core 0** | **3** | Xử lý âm thanh I2S Duplex nội bộ, độc quyền `audio_i2s_tx_mutex` chống va chạm `i2s_write`. |
| **WiFi_Manager** | **Core 0** | **2** | Event-driven, quản lý kết nối, hỗ trợ quên mạng (`forget_network`) và auto-reconnect, lưu NVS ngoài vùng lock_wifi chống deadlock. |
| **Map_Worker** | **Core 0** | **2** | Tải tile HTTP/HTTPS qua FreeRTOS Queue, giải mã JPEG ping-pong buffer vào PSRAM, TLS Root CA bundle. |
| **NetCamWorker** | **Core 0** | **2** | Tải HTTP JPEG Snapshot qua ping-pong double buffer PSRAM, trích xuất metadata thật từ JPEG SOF header. |
| **AI_Voice_Task** | **Core 0** | **2** | Demo/Mock mô phỏng tương tác hội thoại cục bộ (kiểm tra quyền mic trước khi ghi âm). |

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
4. Thông tin xác thực Camera IP (Username / Password / Custom URL) được che chắn tự động (`***:***`) khi build URL và log ra Serial Monitor.

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

1. **System Monitor**: Đo thời gian thực tần số CPU (240MHz), dung lượng RAM nội, 8MB Octal PSRAM, nhiệt độ lõi chip (°C), Uptime (CPU load mô phỏng gắn nhãn Demo).
2. **Google Maps Pro**: Chế độ bản đồ Vector Offline WGS84 kèm chế độ Online Google Satellite / Roadmap Tiles với cơ chế đệm kép Ping-Pong Buffer và API tiêu thụ atomic dưới mutex trong PSRAM.
3. **Music Player**: Trình phát nhạc MP3 điều khiển bằng FreeRTOS Command Queue tuần tự, animation đĩa than, quản lý sở hữu bus I2S độc quyền khi phát nhạc, bảo vệ I/O thẻ nhớ bằng `storage_lock()`.
4. **XiaoZhi AI Voice (Demo/Mock)**: Giao diện chat bong bóng hội thoại phong cách iOS/iMessage, nút Push-To-Talk, kiểm tra quyền sở hữu Micro I2S thực tế, mô phỏng phản hồi giả lập nội bộ (không kết nối API Gemini giả).
5. **WiFi Hub & Control Center**: Quét mạng 2.4GHz, ghi nhớ mạng với Preferences thread-safe, loại bỏ deadlock giữa wifi_mutex và prefs_mutex, không ghi đè NVS khi tự động kết nối từ cấu hình cũ.
6. **Sensors & Telemetry**: Dữ liệu cảm biến la bàn/IMU/áp suất được gắn nhãn rõ ràng là **Demo / Mock** (phần cứng không gắn cảm biến vật lý).
7. **Camera Subsystem**:
   - **HTTP Snapshot (JPEG)**: `READY` (Nhập cấu hình IP/Port/User/Pass trên UI, tải ảnh tĩnh qua mạng, giải mã bằng TJpg_Decoder và hiển thị trực tiếp lên LVGL Canvas kèm đo FPS thực tế).
   - **ONVIF Client**: `NOT_IMPLEMENTED` (Hỗ trợ cấu trúc SOAP cơ bản, không trả kết quả thành công giả khi chưa parse được profile).
   - **MJPEG HTTP Stream**: `NOT_IMPLEMENTED`.
   - **RTSP / H.264 Client**: `NOT_IMPLEMENTED`.
8. **Battery & Power Management**: Đọc ADC điện áp pin trên GPIO 9 của ES3C28P, tự động ẩn trên bo mạch không hỗ trợ (DIYMORE pin = -1), hiển thị trạng thái `Uncalibrated` khi chưa cấu hình hệ số phân áp phần cứng thực tế.
9. **Typography & Vietnamese Localization**: Hệ thống phông chữ UI tùy chỉnh kích thước 10, 12, 14, 16 được tạo từ công cụ `tools/generate_fonts.py` dựa trên font mã nguồn mở **Montserrat** và **Be Vietnam Pro** (bản quyền theo giấy phép **SIL Open Font License 1.1**), hỗ trợ đầy đủ các dải Unicode tiếng Việt có dấu.


---

## ⚖️ 8. Giấy phép mã nguồn
- Mã nguồn firmware được phát hành theo giấy phép **MIT License**.
- Các phông chữ giao diện được phát hành theo giấy phép **SIL Open Font License 1.1**.
