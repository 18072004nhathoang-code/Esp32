# ESP32-S3 ES3C28P Touch Display Mini OS Pro Max
**Kiến trúc:** ES3C28P Hardware Profile • FreeRTOS Multi-tasking • LVGL 8.3.11 • LovyanGFX 1.1.16 DMA • ESP32-audioI2S 3.0.12 • SDMMC Storage • XiaoZhi AI Voice

[![Build Status](https://github.com/18072004nhathoang-code/Esp32/actions/workflows/build.yml/badge.svg)](https://github.com/18072004nhathoang-code/Esp32/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-blue)](https://www.espressif.com/)

---

## 🚀 1. Tổng quan hệ thống

Dự án firmware Mini OS Pro Max chỉ hỗ trợ bo mạch ESP32-S3 ES3C28P 2.8" IPS HMI. Môi trường biên dịch duy nhất trong `platformio.ini` là `esp32-s3-es3c28p`.

**ES3C28P 2.8" IPS HMI**:

- Bo mạch thông minh chuyên dụng trợ lý ảo AI (Xiaozhi/ChatGPT, Cheap Black Display).
- Màn hình 2.8 inch IPS panel native/logical **240x320 Portrait Flipped** (`BOARD_LCD_ROTATION 2`).
- Cảm ứng điện dung đa điểm FocalTech FT6336G (I2C `0x38`) với mapping board cố định và màn hình chẩn đoán thụ động 40Hz (raw/mapped/contact ID), không calibration.
- Thẻ nhớ MicroSD kết nối qua **SDMMC / SDIO chuyên dụng** (không chia sẻ bus với màn hình).
- Âm thanh Codec ES8311 + IC khuếch đại PA FM8002E (Active LOW) + Micro MEMS tích hợp.
- Không có cổng camera DVP vật lý (`BOARD_HAS_LOCAL_CAMERA 0`) -> Tự động chuyển toàn diện sang Network IP Camera (Hikvision, KBVision, Ezviz, Yoosee, ONVIF).

```text
+-------------------------------------------------------------------------------+
|             Mini OS Desktop & Application Layer (240x320 Portrait Flipped)    |
| [Status Bar] [System Monitor] [Google Maps] [Music Player] [XiaoZhi AI Voice] |
|          [WiFi Settings] [Control Center] [Power Manager] [Camera IP]         |
+-------------------------------------------------------------------------------+
|                     LVGL 8.3.11 High-Level Graphics Engine                    |
|      (Be Vietnam Pro SemiBold Typography, 240x320 Portrait, Fast 60 FPS)     |
+-------------------------------------------------------------------------------+
|                 FreeRTOS Multi-Tasking & Thread-Safe Porting                  |
|  - Core 1: LVGL GUI Engine (Priority 4, 12KB Stack, Mutex Protected)          |
|  - Core 0: I2S Audio Engine / MP3 Decoder (Priority 3, Dynamic Arbiter)       |
|  - Core 0: WiFi Service & Auto-Reconnect (Priority 2, Exp-Backoff 2s-60s)    |
|  - Core 0: Storage Manager (SDMMC / SDIO)                                      |
+-------------------------------------------------------------------------------+
|              Hardware Abstraction Layer (include/board_config.h)               |
|  - board_es3c28p.hpp       : ILI9341V (240x320 Portrait Flipped) + FT6336G    |
+-------------------------------------------------------------------------------+
|       Hardware: ESP32-S3-WROOM-1 N16R8 (Dual-Core LX7 @ 240MHz, 16M/8M OPI)   |
+-------------------------------------------------------------------------------+
```

---

## ⚡ 2. Sơ đồ chân phần cứng (Pinout Mapping)

### Bo mạch ES3C28P 2.8" IPS HMI

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

> [!IMPORTANT]
> **Shared I2C Bus (ES3C28P)**: Màn hình cảm ứng FT6336G (`0x38`) và Audio Codec ES8311 (`0x18`) chia sẻ cùng chân GPIO 16 (SDA) và GPIO 15 (SCL). Hệ thống sử dụng module `shared_i2c_bus` với duy nhất một Wire controller được đồng bộ bằng FreeRTOS Mutex (`shared_i2c_lock` / `shared_i2c_unlock`), ngăn chặn xung đột driver hoặc tranh chấp bus.

---

## 📦 3. Thư viện phụ thuộc (Pinned Dependencies)

Tất cả các thư viện trong `platformio.ini` được khóa phiên bản chính xác để đảm bảo tính tái lặp và ổn định tuyệt đối của bản build:

| Thư viện | Phiên bản cố định | Mục đích sử dụng |
| :--- | :--- | :--- |
| `lvgl/lvgl` | **8.3.11** | Nhân giao diện đồ họa chính |
| `lovyan03/LovyanGFX` | **1.1.16** | Driver đồ họa ILI9341V SPI và touch controller |
| `madhephaestus/ESP32Encoder` | **0.11.7** | Đọc rotary encoder nếu có ngoại vi |
| `ESP32-audioI2S` | **3.0.12** (Git commit `#3.0.12`) | Giải mã MP3 từ thẻ nhớ SD qua I2S |
| `bodmer/TJpg_Decoder` | **1.1.0** | Giải mã ảnh JPEG Google Maps & Camera Snapshot vào PSRAM |

---

## 🧵 4. Phân bổ tài nguyên FreeRTOS (Core & Priority)

| Tên Luồng / Task | Nhân Core | Priority | Cơ chế vận hành & Vai trò |
| :--- | :--- | :--- | :--- |
| **LVGL_Task** | **Core 1** | **4** | Chu kỳ 10ms, cập nhật UI, xử lý chạm cảm ứng qua `lvgl_port_lock()`. |
| **MusicAudioTask (MP3)** | **Core 0** | **3** | Nhận lệnh qua FreeRTOS Queue, giải mã MP3, đồng bộ `audio_mutex` và `storage_lock` an toàn tuyệt đối không deadlock. |
| **Audio_Task (AudioManager)** | **Core 0** | **3** | Đọc DMA theo số stereo frame thực nhận, ghi bù partial I2S không lặp frame, snapshot bản thu bất biến có lease và mutex trạng thái STARTING/ACTIVE. |
| **WiFi_Manager** | **Core 0** | **2** | Event-driven, quản lý kết nối, hỗ trợ quên mạng (`forget_network`) và auto-reconnect, lưu NVS ngoài vùng lock_wifi chống deadlock. |
| **Map_Worker** | **Core 0** | **2** | Tải tile HTTP/HTTPS qua FreeRTOS Queue, giải mã JPEG ping-pong buffer vào PSRAM, TLS Root CA bundle. |
| **NetCamWorker** | **Core 0** | **2** | Tải HTTP JPEG Snapshot qua ping-pong double buffer PSRAM, trích xuất metadata thật từ JPEG SOF header. |
| **AI_Voice_Task** | **Core 0** | **2** | Request ID riêng, trạng thái CANCELING/ACK, upload lease WAV bất biến, dechunk bằng HTTPClient và giới hạn JSON 16KB/TTS 2MB trước khi parse/phát. |

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
   #define AI_VOICE_ENDPOINT           "https://voice-gateway.example/v1/query"
   #define AI_VOICE_TTS_ENDPOINT       "https://voice-gateway.example/v1/tts"
   #define AI_VOICE_BEARER_TOKEN       "..."
   #define AI_VOICE_CA_CERT            "-----BEGIN CERTIFICATE-----..."
   ```
3. File `include/secrets.h` đã được thêm vào `.gitignore` để bảo vệ an toàn thông tin cá nhân.
4. **Bảo mật mật khẩu Camera IP**: Firmware không lưu plaintext password vào NVS Flash. Sau reboot, profile có username nhưng thiếu password chuyển sang `PASSWORD_REQUIRED`. HTTPS xác thực bằng `CAMERA_TLS_CA_CERT` là mặc định và fail-closed nếu chưa cấu hình CA. HTTPS bỏ xác thực và HTTP plaintext chỉ hoạt động khi người dùng chọn rõ trong UI; không có downgrade tự động. URL/credential không được ghi plaintext vào log.

---

## 🛠️ 6. Biên dịch và Nạp Firmware

```bash
# Biên dịch firmware cho bo mạch ES3C28P 2.8" IPS HMI (Shopee / Xiaozhi)
pio run -e esp32-s3-es3c28p

# Nạp firmware vào bo mạch ES3C28P
pio run -e esp32-s3-es3c28p -t upload

# Mở Serial Monitor để theo dõi hệ thống (115200 baud)
pio device monitor -b 115200
```

---

## 📱 7. Các phân hệ ứng dụng

1. **System Monitor**: Đọc tần số CPU, tải CPU ước lượng từ idle hooks của hai core, heap/PSRAM, nhiệt độ chip, uptime 64-bit, dung lượng MicroSD, WiFi/RSSI và số task FreeRTOS thật.
2. **Network Map**: Tải ảnh bản đồ HTTPS từ Google Static Maps khi có key, hoặc roadmap OpenStreetMap khi không có key; hỗ trợ pan/zoom, cache MicroSD, ping-pong RGB565 và empty/error state khi mất mạng. Satellite bị vô hiệu hóa nếu chưa cấu hình Google key.
3. **Music Player**: Quét file MP3 thật trong `/music`, phát/tạm dừng/tua/chuyển bài qua command queue, lấy thời lượng từ decoder, quản lý độc quyền I2S và khóa I/O MicroSD.
4. **AI Voice**: Thu PCM thật từ micro, đóng gói WAV từ snapshot có lease và POST tới `AI_VOICE_ENDPOINT`; gateway phải trả JSON `transcript`/`reply` hợp lệ, tối đa 16KB sau dechunk. TTS tối đa 2MB sau dechunk và chỉ được phát khi RIFF hoàn chỉnh, PCM16 mono 16kHz, data chunk không cắt. Request cũ bị loại theo request ID; cancel giữ trạng thái `CANCELING` tới khi worker ACK. Cả hai endpoint bắt buộc HTTPS với CA và bearer token; firmware không log secret.
5. **WiFi Hub & Control Center**: Quét mạng 2.4GHz, kết nối/ngắt/quên mạng, ghi nhớ credential trong NVS, báo lỗi scan/connect/NVS thật và hỗ trợ auto-reconnect có backoff.
6. **Sensors & Diagnostics**: Đọc trạng thái thật của FT6336, ES8311, MicroSD và camera DVP. Profile ES3C28P không khai báo IMU/la bàn/barometer nên UI vô hiệu hóa và báo “không khả dụng”, không sinh số đo giả.
7. **Camera Subsystem**:
   - **HTTP Snapshot (JPEG)**: `READY` (Nhập cấu hình IP/Port/User/Pass trên UI, tải ảnh tĩnh qua mạng, kiểm tra SOI/EOI và giới hạn 512KB, worker duy nhất sở hữu allocate/decode/swap/free ping-pong buffer với capacity front/back riêng, session ID loại frame/lệnh cũ và control mailbox retry tới ACK khi close/reopen hoặc queue thường đầy).
   - **ONVIF/MJPEG/RTSP**: không được bật trong UI vì firmware chưa có decoder/protocol hoàn chỉnh; chỉ Snapshot HTTP(S) được cho phép.
8. **Settings & Power**: Độ sáng, accent, auto-reconnect và timeout dim/display-sleep được lưu NVS. Power đọc ADC pin nếu board khai báo, báo `Uncalibrated` khi hệ số chia áp chưa xác minh, vô hiệu hóa pin/sạc trên board không có driver, đồng thời cung cấp display sleep và restart thật.
9. **Typography & Vietnamese Localization**: Hệ thống phông chữ UI tùy chỉnh kích thước 10, 12, 14, 16 được tạo từ công cụ `tools/generate_fonts.py` dựa trên font mã nguồn mở **Be Vietnam Pro SemiBold** (bản quyền theo giấy phép **SIL Open Font License 1.1**), hỗ trợ đầy đủ các dải Unicode tiếng Việt có dấu, ký tự số và biểu tượng hệ thống. Bố cục chữ trên màn hình hiển thị đậm nét, dễ đọc (`UI_FONT_SMALL` 12px, `UI_FONT_BODY` 14px, `UI_FONT_BUTTON` 14px, `UI_FONT_TITLE` 16px), không phụ thuộc font runtime ngoài.
10. **Touch Architecture & Diagnostic**: Một reader FT6336 dùng shared-I2C mutex, parse TD_STATUS/event/ID, theo đúng một contact ID tới lúc phát release, loại mẫu ngoài native range rồi áp dụng board normalization và rotation đúng một lần. Với profile hiện tại, invert X/Y của sensor rồi rotation 2 triệt tiêu nhau nên mapping cuối là `screen_x=raw_x`, `screen_y=raw_y` trong miền 240x320. LVGL luôn nhận tọa độ toàn màn hình; Touch Diagnostic đổi screen→local theo origin của overlay nội dung, chạy thụ động 40Hz, không calibration/NVS và không chặn boot.

Regression gồm behavioral test, native C++ contract test dùng chung header với firmware và self-test C++ được build vào firmware. Các case bao phủ transform/contact ID, content origin, start audio đồng thời/nhịp DMA, camera session + mailbox ACK, AI request cancel/stale response, giới hạn body và WAV lỗi/cắt.


---

## ⚖️ 8. Giấy phép mã nguồn
- Mã nguồn firmware được phát hành theo giấy phép **MIT License**.
- Các phông chữ giao diện được phát hành theo giấy phép **SIL Open Font License 1.1**.
