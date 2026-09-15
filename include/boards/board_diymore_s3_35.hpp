/**
 * @file board_diymore_s3_35.hpp
 * @brief Hardware profile: DIYMORE ESP32-S3 3.5" IPS (ST7796 480x320 + FT6336U CTP)
 */

#pragma once
#include "board_base.h"

#define BOARD_PROFILE_NAME          "DIYMORE ESP32-S3 3.5\" IPS"
#define BOARD_PROFILE_MCU           "ESP32-S3 (N16R8)"
#define BOARD_PROFILE_FLASH_MB      16
#define BOARD_PROFILE_PSRAM_MB      8

// --- Display: ST7796 480x320 4-wire SPI ---
#define BOARD_LCD_CONTROLLER        LCD_CTRL_ST7796
#define BOARD_LCD_BUS               LCD_BUS_SPI
#define BOARD_LCD_WIDTH             480
#define BOARD_LCD_HEIGHT            320
#define BOARD_LCD_PANEL_WIDTH       320     // Native panel width
#define BOARD_LCD_PANEL_HEIGHT      480     // Native panel height
#define BOARD_LCD_MOSI              11
#define BOARD_LCD_MISO              13
#define BOARD_LCD_SCK               12
#define BOARD_LCD_CS                10
#define BOARD_LCD_DC                4
#define BOARD_LCD_RST               -1
#define BOARD_LCD_BL                45
#define BOARD_LCD_INVERT            true
#define BOARD_LCD_RGB_ORDER         false
#define BOARD_LCD_ROTATION          1       // Landscape mode (480x320)
#define BOARD_LCD_SPI_FREQ          40000000

// --- Touch: FocalTech FT6336U (Capacitive I2C) ---
#define BOARD_TOUCH_CONTROLLER      TOUCH_CTRL_FT6336
#define BOARD_TOUCH_I2C_ADDR        0x38
#define BOARD_TOUCH_SDA             8
#define BOARD_TOUCH_SCL             9
#define BOARD_TOUCH_INT             -1      // Polling mode (tránh xung đột với GPIO 4)
#define BOARD_TOUCH_RST             3

// --- MicroSD Card: SPI Bus dùng chung với LCD (FSPI) ---
#define BOARD_SD_INTERFACE          SD_IF_SPI
#define BOARD_SD_SHARED_SPI         true
#define BOARD_SD_CS                 42
#define BOARD_SD_MOSI               11
#define BOARD_SD_MISO               13
#define BOARD_SD_SCK                12

// --- Audio: Codec ES8311 + Khuếch đại PA Loa ngoài ---
#define BOARD_AUDIO_CODEC           AUDIO_CODEC_ES8311
#define BOARD_AUDIO_I2S_BCLK        18
#define BOARD_AUDIO_I2S_WS          21
#define BOARD_AUDIO_I2S_DOUT        15
#define BOARD_AUDIO_I2S_DIN         16
#define BOARD_AUDIO_I2S_MCLK        17
#define BOARD_AUDIO_PA_PIN          1       // Active LOW (0 = On, 1 = Off)
#define BOARD_AUDIO_I2C_SDA         38
#define BOARD_AUDIO_I2C_SCL         39
#define BOARD_AUDIO_ES8311_ADDR     0x18

// --- Peripherals ---
#define BOARD_BOOT_PIN              0
#define BOARD_RGB_LED_PIN           -1
#define BOARD_BATTERY_ADC_PIN       -1
#define BOARD_BATTERY_CALIBRATED    false
#define BOARD_HAS_LOCAL_CAMERA      0
