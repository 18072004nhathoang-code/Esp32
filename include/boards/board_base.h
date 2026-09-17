/**
 * @file board_base.h
 * @brief Định nghĩa các enum, cờ khả năng phần cứng và macro dùng chung cho HAL
 */

#pragma once

#include <stdint.h>

// Loại giao tiếp màn hình
#define LCD_BUS_SPI            1
#define LCD_BUS_QSPI           2
#define LCD_BUS_RGB_PARALLEL   3
#define LCD_BUS_I8080          4

// Loại IC điều khiển màn hình
#define LCD_CTRL_ILI9341       2
#define LCD_CTRL_ST7789        3
#define LCD_CTRL_ILI9488       4
#define LCD_CTRL_ST7701        5
#define LCD_CTRL_NV3041A       6

// Loại chip cảm ứng
#define TOUCH_CTRL_NONE        0
#define TOUCH_CTRL_FT6336      1
#define TOUCH_CTRL_FT5X06      2
#define TOUCH_CTRL_GT911       3
#define TOUCH_CTRL_CST816      4
#define TOUCH_CTRL_XPT2046     5

// Loại giao tiếp thẻ nhớ MicroSD
#define SD_IF_NONE             0
#define SD_IF_SDMMC_1BIT       2
#define SD_IF_SDMMC_4BIT       3

// Loại Audio Codec
#define AUDIO_CODEC_NONE       0
#define AUDIO_CODEC_ES8311     1
#define AUDIO_CODEC_ES8388     2
#define AUDIO_CODEC_MAX98357A  3
#define AUDIO_CODEC_NS4168     4
