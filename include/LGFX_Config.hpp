/**
 * @file LGFX_Config.hpp
 * @brief Cấu hình phần cứng LovyanGFX siêu tốc tự động thích ứng qua Hardware Abstraction Layer
 * Hỗ trợ ES3C28P (ILI9341V 240x320) và DIYMORE ESP32-S3 3.5" (ST7796 480x320)
 */

#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "board_config.h"

// ==============================================================================
// LỚP DRIVER TỐI ƯU LOVYANGFX (ESP32-S3 HIGH-SPEED DMA)
// ==============================================================================
class LGFX : public lgfx::LGFX_Device
{
#if (BOARD_LCD_CONTROLLER == LCD_CTRL_ILI9341)
    lgfx::Panel_ILI9341 _panel_instance;    // Màn hình ILI9341 / ILI9341V (ES3C28P 2.8")
#elif (BOARD_LCD_CONTROLLER == LCD_CTRL_ST7796)
    lgfx::Panel_ST7796  _panel_instance;    // Màn hình ST7796 IPS (DIYMORE 3.5")
#elif (BOARD_LCD_CONTROLLER == LCD_CTRL_ST7789)
    lgfx::Panel_ST7789  _panel_instance;    // Màn hình ST7789
#else
    lgfx::Panel_ILI9341 _panel_instance;
#endif

    lgfx::Bus_SPI       _bus_instance;      // Giao tiếp SPI phần cứng
    lgfx::Light_PWM     _light_instance;    // PWM LED Backlight

#if (BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_XPT2046)
    lgfx::Touch_XPT2046 _touch_instance;    // Driver cảm ứng điện trở SPI
#elif (BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_GT911)
    lgfx::Touch_GT911   _touch_instance;    // Driver cảm ứng điện dung Goodix GT911
#elif (BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_CST816)
    lgfx::Touch_CSTxxx  _touch_instance;    // Driver cảm ứng điện dung Hynitron CST816S
#elif (BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_FT6336 || BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_FT5X06)
    lgfx::Touch_FT5x06  _touch_instance;    // Driver cảm ứng điện dung FocalTech FT6336U / FT6336G
#else
    lgfx::Touch_FT5x06  _touch_instance;
#endif

public:
    void waitDMA(void)
    {
        _bus_instance.wait();
    }

    LGFX(void)
    {
        {
            // 1. Cấu hình SPI Bus DMA
            auto cfg = _bus_instance.config();
            cfg.spi_host    = SPI2_HOST;            // FSPI trên ESP32-S3
            cfg.spi_mode    = 0;
            cfg.freq_write  = BOARD_LCD_SPI_FREQ;   // Tần số ghi màn hình (40MHz)
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;      // Tự động cấp kênh DMA phần cứng
            cfg.pin_sclk    = BOARD_LCD_SCK;
            cfg.pin_mosi    = BOARD_LCD_MOSI;
            cfg.pin_miso    = BOARD_LCD_MISO;
            cfg.pin_dc      = BOARD_LCD_DC;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            // 2. Cấu hình LCD Panel
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = BOARD_LCD_CS;
            cfg.pin_rst          = BOARD_LCD_RST;
            cfg.pin_busy         = -1;
            cfg.panel_width      = BOARD_LCD_PANEL_WIDTH;
            cfg.panel_height     = BOARD_LCD_PANEL_HEIGHT;
            cfg.invert           = BOARD_LCD_INVERT;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.rgb_order        = BOARD_LCD_RGB_ORDER;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = false;
            _panel_instance.config(cfg);
        }

        {
            // 3. Cấu hình Backlight PWM
            auto cfg = _light_instance.config();
            cfg.pin_bl      = BOARD_LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;                // Tần số PWM 44.1kHz
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

#if (BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_XPT2046)
        {
            // Cấu hình Cảm ứng điện trở XPT2046 (SPI)
            auto cfg = _touch_instance.config();
            cfg.x_min      = 300;
            cfg.x_max      = 3900;
            cfg.y_min      = 300;
            cfg.y_max      = 3900;
            cfg.pin_int    = BOARD_TOUCH_INT;
            cfg.bus_shared = true;
            cfg.spi_host   = SPI2_HOST;
            cfg.freq       = 2500000;
            cfg.pin_sclk   = BOARD_LCD_SCK;
            cfg.pin_mosi   = BOARD_LCD_MOSI;
            cfg.pin_miso   = BOARD_LCD_MISO;
            cfg.pin_cs     = BOARD_TOUCH_CS;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#elif (BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_GT911)
        {
            // Cấu hình Cảm ứng điện dung Goodix GT911 (I2C)
            auto cfg = _touch_instance.config();
            cfg.x_min      = 0;
            cfg.x_max      = BOARD_LCD_PANEL_WIDTH - 1;
            cfg.y_min      = 0;
            cfg.y_max      = BOARD_LCD_PANEL_HEIGHT - 1;
            cfg.pin_int    = BOARD_TOUCH_INT;
            cfg.bus_shared = false;
            cfg.i2c_port   = 1;
            cfg.i2c_addr   = BOARD_TOUCH_I2C_ADDR;
            cfg.pin_sda    = BOARD_TOUCH_SDA;
            cfg.pin_scl    = BOARD_TOUCH_SCL;
            cfg.pin_rst    = BOARD_TOUCH_RST;
            cfg.freq       = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#elif (BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_CST816)
        {
            // Cấu hình Cảm ứng điện dung Hynitron CST816S (I2C)
            auto cfg = _touch_instance.config();
            cfg.x_min      = 0;
            cfg.x_max      = BOARD_LCD_PANEL_WIDTH - 1;
            cfg.y_min      = 0;
            cfg.y_max      = BOARD_LCD_PANEL_HEIGHT - 1;
            cfg.pin_int    = BOARD_TOUCH_INT;
            cfg.bus_shared = false;
            cfg.i2c_port   = 1;
            cfg.i2c_addr   = BOARD_TOUCH_I2C_ADDR;
            cfg.pin_sda    = BOARD_TOUCH_SDA;
            cfg.pin_scl    = BOARD_TOUCH_SCL;
            cfg.pin_rst    = BOARD_TOUCH_RST;
            cfg.freq       = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#elif (BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_FT6336 || BOARD_TOUCH_CONTROLLER == TOUCH_CTRL_FT5X06)
        {
            // Cấu hình Cảm ứng điện dung FocalTech FT6336U / FT6336G (I2C)
            auto cfg = _touch_instance.config();
            cfg.x_min      = 0;
            cfg.x_max      = BOARD_LCD_PANEL_WIDTH - 1;
            cfg.y_min      = 0;
            cfg.y_max      = BOARD_LCD_PANEL_HEIGHT - 1;
            cfg.pin_int    = BOARD_TOUCH_INT;
            cfg.bus_shared = false;
            cfg.i2c_port   = 1;
            cfg.i2c_addr   = BOARD_TOUCH_I2C_ADDR;
            cfg.pin_sda    = BOARD_TOUCH_SDA;
            cfg.pin_scl    = BOARD_TOUCH_SCL;
            cfg.pin_rst    = BOARD_TOUCH_RST;
            cfg.freq       = 400000;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
#endif

        setPanel(&_panel_instance);
    }
};
