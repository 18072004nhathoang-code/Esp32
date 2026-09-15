/**
 * @file board_config.h
 * @brief Master Hardware Abstraction Layer & Compile-time Pin Validation
 */

#pragma once

// Tự động chọn profile theo build flag trong platformio.ini
#if defined(BOARD_DIYMORE_S3_35C)
    #include "boards/board_diymore_s3_35.hpp"
#elif defined(BOARD_ES3C28P)
    #include "boards/board_es3c28p.hpp"
#else
    // Mặc định chọn ES3C28P nếu không chỉ định
    #define BOARD_ES3C28P
    #include "boards/board_es3c28p.hpp"
#endif

// ==============================================================================
// COMPILE-TIME PIN CONFLICT VALIDATION (Section 11)
// ==============================================================================

// 1. Kiểm tra xung đột LCD Chip Select và DC
#if defined(BOARD_LCD_CS) && defined(BOARD_LCD_DC) && (BOARD_LCD_CS >= 0) && (BOARD_LCD_CS == BOARD_LCD_DC)
    #error "[PIN CONFLICT] LCD_CS and LCD_DC cannot share the same GPIO!"
#endif

// 2. Kiểm tra xung đột LCD Clock và DC/CS
#if defined(BOARD_LCD_SCK) && defined(BOARD_LCD_DC) && (BOARD_LCD_SCK == BOARD_LCD_DC)
    #error "[PIN CONFLICT] LCD_SCK and LCD_DC cannot share the same GPIO!"
#endif
#if defined(BOARD_LCD_SCK) && defined(BOARD_LCD_CS) && (BOARD_LCD_SCK == BOARD_LCD_CS)
    #error "[PIN CONFLICT] LCD_SCK and LCD_CS cannot share the same GPIO!"
#endif

// 3. Kiểm tra Touch I2C vs LCD Control (trừ khi cố ý)
#if defined(BOARD_TOUCH_SDA) && defined(BOARD_LCD_DC) && (BOARD_TOUCH_SDA >= 0) && (BOARD_TOUCH_SDA == BOARD_LCD_DC)
    #error "[PIN CONFLICT] TOUCH_SDA conflicts with LCD_DC!"
#endif
#if defined(BOARD_TOUCH_SCL) && defined(BOARD_LCD_DC) && (BOARD_TOUCH_SCL >= 0) && (BOARD_TOUCH_SCL == BOARD_LCD_DC)
    #error "[PIN CONFLICT] TOUCH_SCL conflicts with LCD_DC!"
#endif
#if defined(BOARD_TOUCH_INT) && defined(BOARD_LCD_DC) && (BOARD_TOUCH_INT >= 0) && (BOARD_TOUCH_INT == BOARD_LCD_DC)
    #error "[PIN CONFLICT] TOUCH_INT conflicts with LCD_DC!"
#endif

// 4. Kiểm tra Audio I2S vs Touch I2C (I2S không được trùng các chân I2C độc lập)
#if defined(BOARD_AUDIO_I2S_BCLK) && defined(BOARD_TOUCH_SDA) && (BOARD_AUDIO_I2S_BCLK == BOARD_TOUCH_SDA)
    #error "[PIN CONFLICT] AUDIO_I2S_BCLK conflicts with TOUCH_SDA!"
#endif
#if defined(BOARD_AUDIO_I2S_DOUT) && defined(BOARD_TOUCH_SCL) && (BOARD_AUDIO_I2S_DOUT == BOARD_TOUCH_SCL)
    #error "[PIN CONFLICT] AUDIO_I2S_DOUT conflicts with TOUCH_SCL!"
#endif

// 5. Kiểm tra Audio I2C vs Touch I2C:
// Nếu cùng GPIO thì phải cùng sử dụng bus I2C (hợp lệ). Nếu khác GPIO trên cùng chip thì cảnh báo hoặc xác nhận.
#if defined(BOARD_AUDIO_I2C_SDA) && defined(BOARD_TOUCH_SDA) && (BOARD_AUDIO_I2C_SDA != BOARD_TOUCH_SDA)
    // Cả 2 dùng I2C riêng biệt (hợp lệ trên DIYMORE: Touch=8/9, Audio=38/39)
#endif

// 6. Kiểm tra Backlight vs Audio PA
#if defined(BOARD_LCD_BL) && defined(BOARD_AUDIO_PA_PIN) && (BOARD_LCD_BL == BOARD_AUDIO_PA_PIN)
    #error "[PIN CONFLICT] LCD_BL conflicts with AUDIO_PA_PIN!"
#endif

// ==============================================================================
// EXPORT COMMON MACROS TO MATCH EXISTING CODEBASE
// ==============================================================================

#ifndef DISP_HOR_RES
#define DISP_HOR_RES            BOARD_LCD_WIDTH
#endif

#ifndef DISP_VER_RES
#define DISP_VER_RES            BOARD_LCD_HEIGHT
#endif

#ifndef AUDIO_I2S_BCLK
#define AUDIO_I2S_BCLK          BOARD_AUDIO_I2S_BCLK
#endif

#ifndef AUDIO_I2S_WS
#define AUDIO_I2S_WS            BOARD_AUDIO_I2S_WS
#endif

#ifndef AUDIO_I2S_DOUT
#define AUDIO_I2S_DOUT          BOARD_AUDIO_I2S_DOUT
#endif

#ifndef AUDIO_I2S_DIN
#define AUDIO_I2S_DIN           BOARD_AUDIO_I2S_DIN
#endif

#ifndef AUDIO_I2S_MCLK
#define AUDIO_I2S_MCLK          BOARD_AUDIO_I2S_MCLK
#endif

#ifndef AUDIO_PA_PIN
#define AUDIO_PA_PIN            BOARD_AUDIO_PA_PIN
#endif

#ifndef AUDIO_I2C_SDA
#define AUDIO_I2C_SDA           BOARD_AUDIO_I2C_SDA
#endif

#ifndef AUDIO_I2C_SCL
#define AUDIO_I2C_SCL           BOARD_AUDIO_I2C_SCL
#endif

#ifndef AUDIO_ES8311_ADDR
#define AUDIO_ES8311_ADDR       BOARD_AUDIO_ES8311_ADDR
#endif
