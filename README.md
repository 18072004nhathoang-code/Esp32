# DIYMORE ESP32-S3 3.5" IPS Touch Display Mini OS Pro Max
**Kiến trúc:** FreeRTOS Multi-tasking Embedded OS • LVGL 8.3.11 • LovyanGFX 1.1.16 DMA • ESP32-audioI2S 3.0.12 • XiaoZhi AI Voice

---

## 🚀 1. Tổng quan hệ thống

Dự án firmware Mini OS Pro Max biến bo mạch **DIYMORE ESP32-S3 3.5" IPS (16MB Flash + 8MB Octal PSRAM)** thành một thiết bị cầm tay thông minh đa nhiệm, hỗ trợ giao diện cảm ứng mượt mà 60 FPS, đa nhiệm âm thanh không giật lag, bản đồ trực tuyến/ngoại tuyến có cache thẻ nhớ, trợ lý giọng nói AI Gemini, và kiến trúc sẵn sàng kết nối Camera DVP/RTSP.

```text
+-------------------------------------------------------------------------------+
|                    Mini OS Pro Max Desktop & Application Layer                 |
| [Status Bar] [System Monitor] [Google Maps Pro] [Music Player] [XiaoZhi AI]  |
|          [WiFi Hub] [Settings Control Center] [Sensors] [Camera DVP/RTSP]     |
+-------------------------------------------------------------------------------+
|                     LVGL 8.3.11 High-Level Graphics Engine                    |
|      (Glassmorphism UI, Responsive Widgets, Montserrat Fonts, Animations)     |
+-------------------------------------------------------------------------------+
|                 FreeRTOS Multi-Tasking & Thread-Safe Porting                  |
|  - Core 1: LVGL GUI Engine (Priority 4, 16KB Stack, Mutex Protected)          |
|  - Core 0: I2S Audio Engine / MP3 Decoder (Priority 3, Non-blocking Delay)    |
|  - Core 0: WiFi Service & Auto-Reconnect (Priority 2, Exp-Backoff 2s-60s)    |
|  - Core 0: Map Tile Downloader Queue Worker (Priority 2, PSRAM Stream Buffer) |
|  - Core 0: XiaoZhi AI Voice Streaming (Priority 2, Core 0 FreeRTOS Task)      |
+-------------------------------------------------------------------------------+
|                  Hardware Abstraction & Hardware DMA Acceleration             |
|  - LovyanGFX SPI DMA 40MHz (ST7796 IPS LCD, Hardware WaitDMA Synchronization) |
|  - Shared SPI Bus Controller (Synchronized LCD vs MicroSD FAT32 Bus Access)  |
|  - I2S Duplex Bus Arbiter (Loa ngoài MAX98357A & Micro MEMS INMP441)          |
|  - Hardware PWM Backlight Timer & Power Management Display Sleep Mode         |
+-------------------------------------------------------------------------------+
|            Hardware: DIYMORE ESP32-S3-WROOM-1 N16R8 (Dual-Core LX7 @ 240MHz)   |
+-------------------------------------------------------------------------------+
```

---

## ⚡ 2. Sơ đồ đấu nối chân phần cứng (Pinout Mapping)

### A. Bo mạch mặc định: DIYMORE ESP32-S3 3.5" IPS (ST7796 480x320 Capacitive Touch)

| Module / Ngoại vi | Chức năng tín hiệu | Chân ESP32-S3 (GPIO) | Ghi chú kỹ thuật |
| :--- | :--- | :--- | :--- |
| **Màn hình LCD (ST7796 SPI)** | LCD_MOSI | **GPIO 11** | SPI2_HOST (FSPI), 40MHz DMA |
| | LCD_MISO | **GPIO 13** | Dùng chung bus SPI với MicroSD |
| | LCD_SCLK | **GPIO 12** | Xung nhịp SPI chung |
| | LCD_CS | **GPIO 10** | Chip Select LCD (Active Low) |
| | LCD_DC | **GPIO 4** | Data / Command Select |
| | LCD_RST | **-1** | Nối qua chân RESET chung hoặc EN |
| | LCD_BL | **GPIO 45** (hoặc 16) | Điều khiển độ sáng màn hình LEDC PWM 1.2kHz |
| **Cảm ứng (FT6336U / GT911)** | TOUCH_SDA | **GPIO 17** (hoặc 4) | I2C Data bus |
| | TOUCH_SCL | **GPIO 18** (hoặc 5) | I2C Clock bus |
| | TOUCH_INT | **-1** (Polling) | **Polling I2C** (tránh xung đột với LCD_DC GPIO 4) |
| | TOUCH_RST | **-1** | Tự động reset phần cứng |
| **Thẻ nhớ MicroSD (SPI Slot)** | SD_MOSI | **GPIO 11** | Dùng chung bus SPI (Bảo vệ bằng waitDMA()) |
| | SD_MISO | **GPIO 13** | Dùng chung bus SPI |
| | SD_SCLK | **GPIO 12** | Dùng chung bus SPI |
| | SD_CS | **GPIO 42** (hoặc 5) | Chip Select riêng cho khe cắm thẻ nhớ MicroSD |
| **Âm thanh Loa ngoài (I2S TX)** | I2S_BCLK | **GPIO 15** | Bit Clock DAC MAX98357A |
| | I2S_LRCK | **GPIO 16** | Left/Right Word Select Clock |
| | I2S_DOUT | **GPIO 7** | Serial Audio Data Out |
| | PA_EN | **GPIO 46** | Kích hoạt công suất Audio Amp |
| **Micro thu âm (I2S RX)** | MIC_BCLK | **GPIO 1** | Bit Clock Micro MEMS INMP441 |
| | MIC_WS | **GPIO 2** | Word Select Clock Micro |
| | MIC_DIN | **GPIO 41** | Audio Data Input |

> [!IMPORTANT]
> **Đồng bộ hóa SPI Bus**: LCD ST7796 và MicroSD dùng chung chân SPI (GPIO 11, 12, 13). Module sd_map_cache và music_player đã được tích hợp bộ khóa sd_acquire_bus() gọi gfx.waitDMA(), đảm bảo DMA LCD hoàn tất trước khi đọc/ghi file trên thẻ nhớ MicroSD.

### B. Cấu hình bo mạch khác (Legacy Profiles)
Trong file include/LGFX_Config.hpp, người dùng có thể dễ dàng chuyển đổi sang các dòng bo mạch khác bằng cách uncomment:
- #define BOARD_DIYMORE_S3_35 *(Mặc định)*: Màn hình 3.5" IPS 480x320 ST7796 cảm ứng điện dung.
- #define BOARD_SUNTON_S3_35: Sunton ESP32-3248S035 (3.5" 480x320).
- #define BOARD_SUNTON_S3_28C: Sunton ESP32-2432S028C (2.8" 320x240 cảm ứng điện dung).
- #define BOARD_SUNTON_S3_28R: Sunton ESP32-2432S028R (2.8" 320x240 cảm ứng điện trở XPT2046).

---

## 🧵 3. Kiến trúc Đa luồng FreeRTOS (Task Priority Map)

Hệ thống phân bổ tài nguyên chặt chẽ giữa 2 nhân vi điều khiển ESP32-S3:

| Tên Task | Nhân thực thi | Priority | Chu kỳ / Cơ chế | Vai trò và chức năng |
| :--- | :--- | :--- | :--- | :--- |
| **LVGL_Task** | **Core 1** | **4** | 10ms (100 Hz) | Quản lý rendering đồ họa LVGL 8, touch polling, timer handler, bảo vệ bằng recursive mutex lvgl_port_lock(). |
| **Audio_Task / MusicTask** | **Core 0** | **3** | TaskDelay(2ms) | Giải mã MP3 từ thẻ nhớ SD & thu âm micro, điều phối tài nguyên I2S bằng cờ AudioOwner. Loại bỏ 	askYIELD() để tránh đói CPU. |
| **WiFi_Service** | **Core 0** | **2** | Event-driven | Giám sát kết nối WiFi, tự động kết nối lại theo thuật toán Exponential Backoff (2s -> 60s), ghi nhớ thông tin mà không gây hao mòn Flash NVS. |
| **Map_Worker** | **Core 0** | **2** | Hàng đợi Queue | Xử lý yêu cầu tải tile bản đồ HTTP/HTTPS qua map_request_queue, quản lý bộ đệm giải mã JPEG trong PSRAM. |
| **AI_Voice_Task** | **Core 0** | **2** | Async HTTP Stream | Thu âm từ mic MEMS, gửi streaming lên OpenAI/Gemini AI endpoint và phát trả lời qua Loa ngoài I2S. |

---

## 🔒 4. Cấu hình Bảo mật và API Keys (Secrets)

Dự án tuyệt đối không lưu cứng (hardcode) mật khẩu mạng và khóa API vào mã nguồn công khai:
1. Sao chép file mẫu:
   `ash
   cp include/secrets.example.h include/secrets.h
   `
2. Điền thông tin cá nhân vào include/secrets.h:
   `c
   #define WIFI_DEFAULT_SSID       "Your_Home_WiFi"
   #define WIFI_DEFAULT_PASS       "Your_Secret_Password"
   #define GOOGLE_MAPS_API_KEY     "AIzaSy..."
   #define GEMINI_API_KEY          "AIzaSy..."
   `
3. File include/secrets.h đã được thêm vào .gitignore để ngăn chặn việc vô tình đẩy khóa API lên Git.

---

## 🛠️ 5. Hướng dẫn Biên dịch và Nạp Firmware (PlatformIO)

### Yêu cầu môi trường:
- VS Code với tiện ích mở rộng **PlatformIO IDE**.
- Hoặc cài đặt PlatformIO Core qua terminal: pip install platformio.

### Lệnh biên dịch và nạp qua dòng lệnh:
`ash
# Biên dịch toàn bộ firmware
pio run

# Nạp firmware vào bo mạch ESP32-S3 qua cổng COM
pio run -t upload

# Mở Serial Monitor để theo dõi log hệ thống (115200 baud)
pio run -t monitor

# Nạp bảng phân vùng (nếu thay đổi partitions.csv)
pio run -t uploadfs
`

---

## 📱 6. Các ứng dụng tích hợp trong hệ thống

1. **System Monitor**: Đo lường thời gian thực tần số CPU (240MHz), dung lượng RAM nội, bộ nhớ ngoài 8MB Octal PSRAM, nhiệt độ lõi chip (°C) và thời gian hoạt động liên tục (Uptime).
2. **Google Maps Pro**: Chế độ bản đồ Vector Offline WGS84 kèm chế độ Online Google Satellite / Roadmap Tiles với cơ chế lưu đệm cache thông minh vào thẻ nhớ MicroSD.
3. **Music Player**: Trình phát nhạc MP3 giao diện chia đôi cột, animation đĩa than quay xoay tròn, thanh trượt âm lượng và giải mã I2S nền không gián đoạn.
4. **XiaoZhi AI Voice Assistant**: Giao diện chat bong bóng hội thoại phong cách iOS/iMessage, nút bấm Push-To-Talk, animation sóng âm động và tích hợp mô hình ngôn ngữ lớn (Gemini).
5. **WiFi Hub & Control Center**: Quét mạng 2.4GHz thời gian thực, giao diện bàn phím nhập mật khẩu kết nối trực quan, điều khiển độ sáng màn hình LEDC PWM 0-100%.
6. **Camera & RTSP Subsystem**: Tầng trừu tượng hóa driver camera DVP 8-bit (OV2640/OV5640), chuẩn bị sẵn sàng cho truyền phát video RTSP/ONVIF và live preview.

---

## ⚖️ 7. Giấy phép mã nguồn
Dự án được phát hành theo giấy phép **MIT License**. Tự do sử dụng, chỉnh sửa và phát triển cho các dự án thương mại và giáo dục.
