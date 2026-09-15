/**
 * @file board_es3c28p.hpp
 * @brief Hardware profile: LCDWIKI / Makerfabs ES3C28P 2.8" IPS
 *        (ILI9341V 240x320 + FT6336G CTP + ES8311 Codec + SDMMC)
 *        Tương thích sản phẩm Shopee: Màn hình HMI ES3C28P 2.8" ESP32-S3
 */

#pragma once
#include "board_base.h"

#define BOARD_PROFILE_NAME          "ES3C28P 2.8\" IPS HMI"
#define BOARD_PROFILE_MCU           "ESP32-S3 (N16R8)"
#define BOARD_PROFILE_FLASH_MB      16
#define BOARD_PROFILE_PSRAM_MB      8

// --- Display: ILI9341V 240x320 Portrait 4-wire SPI ---
#define BOARD_LCD_CONTROLLER        LCD_CTRL_ILI9341
#define BOARD_LCD_BUS               LCD_BUS_SPI
#define BOARD_LCD_WIDTH             240     // Portrait resolution
#define BOARD_LCD_HEIGHT            320
#define BOARD_LCD_PANEL_WIDTH       240     // Panel native resolution
#define BOARD_LCD_PANEL_HEIGHT      320
#define BOARD_LCD_MOSI              11
#define BOARD_LCD_MISO              13
#define BOARD_LCD_SCK               12
#define BOARD_LCD_CS                10
#define BOARD_LCD_DC                46      // IO46 là TFT_RS / DC trên ES3C28P
#define BOARD_LCD_RST               -1      // Nối chân CHIP_PU (EN)
#define BOARD_LCD_BL                45      // IO45 điều khiển đèn nền LCD Backlight
#define BOARD_LCD_INVERT            false   // ILI9341V tiêu chuẩn không invert
#define BOARD_LCD_RGB_ORDER         false   // RGB Order
#define BOARD_LCD_SPI_FREQ          40000000

// --- Touch: FocalTech FT6336G (Capacitive I2C) ---
#define BOARD_TOUCH_CONTROLLER      TOUCH_CTRL_FT6336
#define BOARD_TOUCH_I2C_ADDR        0x38
#define BOARD_TOUCH_SDA             16      // IO16 là TP_SDA
#define BOARD_TOUCH_SCL             15      // IO15 là TP_SCL
#define BOARD_TOUCH_INT             17      // IO17 là TP_INT (Low khi có chạm)
#define BOARD_TOUCH_RST             18      // IO18 là TP_RST (Low level reset)

// --- MicroSD Card: Giao tiếp SDMMC / SDIO chuyên dụng (Không chia sẻ bus với LCD) ---
#define BOARD_SD_INTERFACE          SD_IF_SDMMC_4BIT
#define BOARD_SD_SHARED_SPI         false   // Độc lập hoàn toàn với FSPI của LCD
#define BOARD_SD_CLK                38      // IO38 là SD_CLK
#define BOARD_SD_CMD                40      // IO40 là SD_CMD
#define BOARD_SD_D0                 39      // IO39 là SD_D0
#define BOARD_SD_D1                 41      // IO41 là SD_D1
#define BOARD_SD_D2                 48      // IO48 là SD_D2
#define BOARD_SD_D3                 47      // IO47 là SD_D3

// --- Audio: Codec ES8311 + Amply FM8002E + Micro MEMS tích hợp ---
#define BOARD_AUDIO_CODEC           AUDIO_CODEC_ES8311
#define BOARD_AUDIO_I2S_MCLK        4       // IO4 là I2S_MCK
#define BOARD_AUDIO_I2S_BCLK        5       // IO5 là I2S_SCK
#define BOARD_AUDIO_I2S_DIN         6       // IO6 là I2S_DI (từ Mic)
#define BOARD_AUDIO_I2S_WS          7       // IO7 là I2S_LRC
#define BOARD_AUDIO_I2S_DOUT        8       // IO8 là I2S_DO (ra Loa)
#define BOARD_AUDIO_PA_PIN          1       // IO1 là Audio Output Enable (Active LOW: 0 = On, 1 = Off)
#define BOARD_AUDIO_I2C_SDA         16      // Dùng chung I2C bus với cảm ứng
#define BOARD_AUDIO_I2C_SCL         15      // Dùng chung I2C bus với cảm ứng
#define BOARD_AUDIO_ES8311_ADDR     0x18

// --- Peripherals ---
#define BOARD_BOOT_PIN              0       // IO0 nút BOOT
#define BOARD_RGB_LED_PIN           42      // IO42 là LED RGB đơn tuyến (WS2812)
#define BOARD_BATTERY_ADC_PIN       9       // IO9 là BAT_ADC
#define BOARD_UART_TX_PIN           43      // IO43 là TXD0
#define BOARD_UART_RX_PIN           44      // IO44 là RXD0
#define BOARD_HAS_LOCAL_CAMERA      0       // Không có cổng kết nối camera DVP vật lý
