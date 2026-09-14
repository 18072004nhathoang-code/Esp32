/**
 * @file LGFX_Config.hpp
 * @brief Cấu hình phần cứng LovyanGFX siêu tốc cho màn hình ESP32-S3 3.5 inch Touch Display (480x320)
 * và tương thích ngược với các dòng bo mạch 2.8 inch (ST7789/ILI9341).
 * Hỗ trợ bo mạch DIYMORE ESP32-S3 3.5" IPS (XiaoZhi AI), Sunton ESP32-3248S035, CYD.
 */

#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ==============================================================================
// 1. CHỌN LOẠI BO MẠCH CỦA BẠN (Mở comment 1 dòng tương ứng với phần cứng thực tế)
// ==============================================================================
#define BOARD_DIYMORE_S3_35C    // Bo mạch DIYMORE ESP32-S3 3.5" IPS 480x320 Cảm ứng điện dung (ST7796 - XiaoZhi AI) - MẶC ĐỊNH
// #define BOARD_SUNTON_S3_28R  // Bo mạch Sunton ESP32-S3 2.8" cảm ứng điện trở (XPT2046)
// #define BOARD_SUNTON_S3_28C  // Bo mạch Sunton ESP32-S3 2.8" cảm ứng điện dung (CST816S / GT911)
// #define BOARD_CYD_S3_28      // Bo mạch CYD ESP32-S3 2.8" (Yellow Display S3)
// #define BOARD_CUSTOM_S3      // Tự định nghĩa chân tùy ý

// ==============================================================================
// 2. ĐỊNH NGHĨA CHÂN PHẦN CỨNG (PINOUT MAPPING)
// ==============================================================================
#if defined(BOARD_DIYMORE_S3_35C)
    // --- DIYMORE ESP32-S3 3.5" IPS 480x320 (ST7796 + Cảm ứng điện dung I2C) ---
    #define LCD_MOSI        11
    #define LCD_MISO        13
    #define LCD_SCK         12
    #define LCD_DC          4
    #define LCD_CS          10
    #define LCD_RST         -1      // Nối EN hoặc để -1
    #define LCD_BL          45      // Điều khiển độ sáng đèn nền PWM (hoặc GPIO 48 / 16 tùy revision)

    // Cảm ứng điện dung: Mặc định chip FocalTech FT6336U (đổi sang TOUCH_CONTROLLER_GT911 nếu dùng GT911)
    #define TOUCH_CONTROLLER_FT6336U
    #define TOUCH_SDA       8       // Chân I2C SDA
    #define TOUCH_SCL       9       // Chân I2C SCL
    #define TOUCH_INT       -1      // Chế độ polling qua I2C (tránh xung đột với LCD_DC GPIO 4)
    #define TOUCH_RST       3       // Chân reset

#elif defined(BOARD_SUNTON_S3_28R)
    // --- Sunton ESP32-S3-2432S028R 2.8" (Cảm ứng điện trở) ---
    #define LCD_MOSI        11
    #define LCD_MISO        13
    #define LCD_SCK         12
    #define LCD_DC          4
    #define LCD_CS          10
    #define LCD_RST         -1
    #define LCD_BL          16

    #define TOUCH_CONTROLLER_XPT2046    // Dùng chip cảm ứng điện trở XPT2046 SPI
    #define TOUCH_MOSI      11
    #define TOUCH_MISO      13
    #define TOUCH_SCK       12
    #define TOUCH_CS        33
    #define TOUCH_IRQ       36

#elif defined(BOARD_SUNTON_S3_28C)
    // --- Sunton ESP32-S3-2432S028C 2.8" (Cảm ứng điện dung I2C) ---
    #define LCD_MOSI        11
    #define LCD_MISO        13
    #define LCD_SCK         12
    #define LCD_DC          4
    #define LCD_CS          10
    #define LCD_RST         -1
    #define LCD_BL          16

    #define TOUCH_CONTROLLER_CST816S    // Dùng chip cảm ứng điện dung Hynitron CST816S I2C
    #define TOUCH_SDA       4
    #define TOUCH_SCL       5
    #define TOUCH_INT       0
    #define TOUCH_RST       1

#elif defined(BOARD_CYD_S3_28)
    // --- CYD ESP32-S3 2.8" SPI ---
    #define LCD_MOSI        13
    #define LCD_MISO        12
    #define LCD_SCK         14
    #define LCD_DC          2
    #define LCD_CS          15
    #define LCD_RST         -1
    #define LCD_BL          21

    #define TOUCH_CONTROLLER_XPT2046
    #define TOUCH_MOSI      13
    #define TOUCH_MISO      12
    #define TOUCH_SCK       14
    #define TOUCH_CS        33
    #define TOUCH_IRQ       36

#else // BOARD_CUSTOM_S3
    #define LCD_MOSI        11
    #define LCD_MISO        13
    #define LCD_SCK         12
    #define LCD_DC          4
    #define LCD_CS          10
    #define LCD_RST         -1
    #define LCD_BL          45
    #define TOUCH_CONTROLLER_FT6336U
    #define TOUCH_SDA       8
    #define TOUCH_SCL       9
    #define TOUCH_INT       -1
    #define TOUCH_RST       3
#endif

// ==============================================================================
// 2.1. COMPILE-TIME PIN CONFLICT VALIDATION
// ==============================================================================
#if defined(TOUCH_INT) && (TOUCH_INT >= 0) && (TOUCH_INT == LCD_DC)
    #error "Pin conflict: TOUCH_INT and LCD_DC cannot use the same GPIO!"
#endif
#if defined(TOUCH_SDA) && (TOUCH_SDA >= 0) && (TOUCH_SDA == LCD_DC)
    #error "Pin conflict: TOUCH_SDA and LCD_DC cannot use the same GPIO!"
#endif
#if defined(LCD_CS) && defined(LCD_DC) && (LCD_CS == LCD_DC)
    #error "Pin conflict: LCD_CS and LCD_DC cannot use the same GPIO!"
#endif

// ==============================================================================
// 3. LỚP DRIVER TỐI ƯU LOVYANGFX (ESP32-S3 HIGH-SPEED DMA)
// ==============================================================================
class LGFX : public lgfx::LGFX_Device
{
#if defined(BOARD_DIYMORE_S3_35C)
    lgfx::Panel_ST7796  _panel_instance;    // Màn hình IPS 3.5" dùng IC ST7796
#else
    lgfx::Panel_ST7789  _panel_instance;    // Màn hình 2.8" dùng ST7789 (hoặc ILI9341)
#endif
    lgfx::Bus_SPI       _bus_instance;      // Giao tiếp SPI phần cứng
    lgfx::Light_PWM     _light_instance;    // PWM LED Backlight

#if defined(TOUCH_CONTROLLER_XPT2046)
    lgfx::Touch_XPT2046 _touch_instance;    // Driver cảm ứng điện trở SPI
#elif defined(TOUCH_CONTROLLER_GT911)
    lgfx::Touch_GT911   _touch_instance;    // Driver cảm ứng điện dung Goodix GT911
#elif defined(TOUCH_CONTROLLER_CST816S)
    lgfx::Touch_CSTxxx  _touch_instance;    // Driver cảm ứng điện dung Hynitron CST816S
#else
    lgfx::Touch_FT5x06  _touch_instance;    // Driver cảm ứng điện dung FocalTech FT6336U / FT5x06
#endif

public:
    void waitDMA(void)
    {
        _bus_instance.wait();
    }

    LGFX(void)
    {
        {
            // Cấu hình SPI Bus DMA
            auto cfg = _bus_instance.config();
            cfg.spi_host    = SPI2_HOST;     // FSPI trên ESP32-S3
            cfg.spi_mode    = 0;
            cfg.freq_write  = 40000000;      // Tần số ghi màn hình 40MHz
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO; // Tự động cấp kênh DMA phần cứng
            cfg.pin_sclk    = LCD_SCK;
            cfg.pin_mosi    = LCD_MOSI;
            cfg.pin_miso    = LCD_MISO;
            cfg.pin_dc      = LCD_DC;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            // Cấu hình LCD Panel
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = LCD_CS;
            cfg.pin_rst          = LCD_RST;
            cfg.pin_busy         = -1;
#if defined(BOARD_DIYMORE_S3_35C)
            cfg.panel_width      = 320;      // Màn hình 3.5" (320x480)
            cfg.panel_height     = 480;
            cfg.invert           = true;     // IPS ST7796 thường invert màu
#else
            cfg.panel_width      = 240;      // Màn hình 2.8" (240x320)
            cfg.panel_height     = 320;
            cfg.invert           = false;
#endif
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.rgb_order        = false;    // BGR/RGB
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel_instance.config(cfg);
        }

        {
            // Cấu hình Backlight PWM
            auto cfg = _light_instance.config();
            cfg.pin_bl      = LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;        // Tần số PWM 44.1kHz
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

#if defined(TOUCH_CONTROLLER_XPT2046)
        {
            // Cấu hình Cảm ứng điện trở XPT2046 (SPI)
            auto cfg = _touch_instance.config();
            cfg.x_min      = 300;
            cfg.x_max      = 3900;
            cfg.y_min      = 300;
            cfg.y_max      = 3900;
            cfg.pin_int    = TOUCH_IRQ;
            cfg.bus_shared = true;
            cfg.spi_host   = SPI2_HOST;
            cfg.freq       = 2500000;
            cfg.pin_sclk   = TOUCH_SCK;
            cfg.pin_mosi   = TOUCH_MOSI;
            cfg.pin_miso   = TOUCH_MISO;
            cfg.pin_cs     = TOUCH_CS;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#elif defined(TOUCH_CONTROLLER_GT911)
        {
            // Cấu hình Cảm ứng điện dung Goodix GT911 (I2C)
            auto cfg = _touch_instance.config();
            cfg.x_min      = 0;
            cfg.x_max      = 319;
            cfg.y_min      = 0;
            cfg.y_max      = 479;
            cfg.pin_int    = TOUCH_INT;
            cfg.bus_shared = false;
            cfg.i2c_port   = 1;
            cfg.i2c_addr   = 0x5D;          // Goodix GT911 mặc định 0x5D (hoặc 0x14)
            cfg.pin_sda    = TOUCH_SDA;
            cfg.pin_scl    = TOUCH_SCL;
            cfg.pin_rst    = TOUCH_RST;
            cfg.freq       = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#elif defined(TOUCH_CONTROLLER_CST816S)
        {
            // Cấu hình Cảm ứng điện dung Hynitron CST816S (I2C)
            auto cfg = _touch_instance.config();
            cfg.x_min      = 0;
            cfg.x_max      = 239;
            cfg.y_min      = 0;
            cfg.y_max      = 319;
            cfg.pin_int    = TOUCH_INT;
            cfg.bus_shared = false;
            cfg.i2c_port   = 1;
            cfg.i2c_addr   = 0x15;          // CST816S mặc định 0x15
            cfg.pin_sda    = TOUCH_SDA;
            cfg.pin_scl    = TOUCH_SCL;
            cfg.pin_rst    = TOUCH_RST;
            cfg.freq       = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#else
        {
            // Cấu hình Cảm ứng điện dung FocalTech FT6336U / FT5x06 (I2C)
            auto cfg = _touch_instance.config();
            cfg.x_min      = 0;
            cfg.x_max      = 319;
            cfg.y_min      = 0;
            cfg.y_max      = 479;
            cfg.pin_int    = TOUCH_INT;
            cfg.bus_shared = false;
            cfg.i2c_port   = 1;
            cfg.i2c_addr   = 0x38;          // FocalTech FT6336U mặc định 0x38
            cfg.pin_sda    = TOUCH_SDA;
            cfg.pin_scl    = TOUCH_SCL;
            cfg.pin_rst    = TOUCH_RST;
            cfg.freq       = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#endif

        setPanel(&_panel_instance);
    }
};
