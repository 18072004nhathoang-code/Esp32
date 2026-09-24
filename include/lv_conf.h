/**
 * @file lv_conf.h
 * Configuration file for v8.3.x of LVGL on ESP32-S3
 * Optimized for LovyanGFX Driver & Mini OS
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>
#include <esp_heap_caps.h>

/* Set value to 0 to apply this configuration file */
#define LV_CONF_SKIP 0

/*====================
   COLOR SETTINGS
 *====================*/
/* Color depth: 16 (RGB565) - Chuẩn cho màn hình 2.8 inch */
#define LV_COLOR_DEPTH 16

/* Swap 2 bytes của RGB565.
 * Khi dùng LovyanGFX, driver LGFX xử lý thứ tự byte trực tiếp ở tầng DMA,
 * do đó giữ giá trị này là 0 để đạt hiệu năng tối đa không bị tốn CPU đảo byte. */
#define LV_COLOR_16_SWAP 0

/* 1: Enable 1-bit transparency (chroma keying) */
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00FF00)

/* 1: Enable screen transparency / layer alpha rendering for transform angles (rotating vinyl).
 * Màn hình đầu ra thực tế vẫn là RGB565 (LV_COLOR_DEPTH 16). */
#define LV_COLOR_SCREEN_TRANSP 1

/*=========================
   MEMORY SETTINGS
 *=========================*/
/* 0: Dùng bộ quản lý bộ nhớ tích hợp của LVGL; 1: Dùng malloc tùy biến */
#define LV_MEM_CUSTOM 0
#if LV_MEM_CUSTOM == 0
    /* One deterministic LVGL pool in PSRAM. DMA buffers remain internal. */
    #define LV_MEM_SIZE (96U * 1024U)
    #define LV_MEM_ADR 0
    #define LV_MEM_POOL_INCLUDE <esp_heap_caps.h>
    #define LV_MEM_POOL_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    #define LV_MEM_POOL_FREE(ptr) heap_caps_free((ptr))
#else
    #define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
    #define LV_MEM_CUSTOM_ALLOC   malloc
    #define LV_MEM_CUSTOM_FREE    free
    #define LV_MEM_CUSTOM_REALLOC realloc
#endif

/* Số lượng luồng / buffer tối đa cho render */
#define LV_MEM_BUF_MAX_NUM 16

/*====================
   HAL SETTINGS
 *====================*/
/* Tần số quét màn hình mặc định (16ms tương đương ~60 FPS mượt mà) */
#define LV_DISP_DEF_REFR_PERIOD 16

/* Chu kỳ đọc cảm ứng Touchpad (16ms) */
#define LV_INDEV_DEF_READ_PERIOD 16

/* Sử dụng tick timer từ phần cứng Arduino/FreeRTOS */
#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM
    #define LV_TICK_CUSTOM_INCLUDE <Arduino.h>
    #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

/* DPI chuẩn cho màn hình 2.8 inch 240x320 (~140 DPI) */
#define LV_DPI_DEF 140

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/

/* Drawing */
#define LV_DRAW_COMPLEX 1
#if LV_DRAW_COMPLEX
    #define LV_SHADOW_CACHE_SIZE 2
    #define LV_CIRCLE_CACHE_SIZE 4
#endif

/* Animations */
#define LV_USE_ANIMATION 1
#define LV_USE_SHADOW 1
#define LV_USE_BLEND_MODES 1
#define LV_USE_OPA_SCALE 1
#define LV_USE_IMG_TRANSFORM 1

/* Hiển thị FPS và mức độ chiếm dụng CPU / RAM trên màn hình (Chỉ bật khi có cờ MINI_OS_DEBUG_PERF) */
#ifdef MINI_OS_DEBUG_PERF
    #define LV_USE_PERF_MONITOR 1
    #define LV_USE_PERF_MONITOR_POS LV_ALIGN_BOTTOM_RIGHT
#else
    #define LV_USE_PERF_MONITOR 0
#endif

#define LV_USE_MEM_MONITOR 0

/* The UI uses LVGL's formatted-label API for live temperature, audio level,
 * map coordinates and camera FPS.  Keep float argument consumption enabled;
 * disabling it makes a following %s consume the pending double as a pointer. */
#define LV_SPRINTF_USE_FLOAT 1

/* Logging */
#define LV_USE_LOG 1
#if LV_USE_LOG
    #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
    #define LV_LOG_PRINTF 0
#endif

/*==================
 * FONT USAGE
 *==================*/
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_32 0

/* Font mặc định cho toàn bộ Mini OS */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Hỗ trợ hiển thị biểu tượng FontAwesome có sẵn trong LVGL */
#define LV_USE_FONT_COMPRESSED 1
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_SUBPX 0

/*================
 * THEME USAGE
 *================*/
#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
    /* Giao diện nền tối sang trọng hiện đại */
    #define LV_THEME_DEFAULT_DARK 1
    #define LV_THEME_DEFAULT_GROW 1
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif

#define LV_USE_THEME_BASIC 1

/*==================
 * WIDGETS CONFIG
 *==================*/
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1 /* required by LV_USE_KEYBOARD */
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1 /* required by LV_USE_CANVAS */
#define LV_USE_LABEL      1
#if LV_USE_LABEL
    #define LV_LABEL_TEXT_SELECTION 1
    #define LV_LABEL_LONG_TXT_HINT 1
#endif
#define LV_USE_LINE       0
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      0

/* Extra Widgets (Phục vụ xây dựng các App của Mini OS) */
#define LV_USE_ANIMIMG    0
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      1
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_KEYBOARD   1
#define LV_USE_LED        0
#define LV_USE_LIST       1
#define LV_USE_MENU       0
#define LV_USE_METER      0
#define LV_USE_MSGBOX     0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_SPAN       0

#endif /*LV_CONF_H*/
