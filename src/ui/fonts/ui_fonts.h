/**
 * @file ui_fonts.h
 * @brief Vietnamese Unicode & ASCII custom fonts for ESP32-S3 Mini OS
 * Supports full ASCII, Latin-1, Latin Extended A/B, Latin Extended Additional (full Vietnamese diacritics),
 * punctuation, and fallback to Montserrat symbols.
 */

#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t ui_font_10;
extern const lv_font_t ui_font_12;
extern const lv_font_t ui_font_14;
extern const lv_font_t ui_font_16;

#define UI_FONT_10 (&ui_font_10)
#define UI_FONT_12 (&ui_font_12)
#define UI_FONT_14 (&ui_font_14)
#define UI_FONT_16 (&ui_font_16)

#define UI_FONT_SMALL  (&ui_font_10)
#define UI_FONT_BODY   (&ui_font_14)
#define UI_FONT_BUTTON (&ui_font_14)
#define UI_FONT_TITLE  (&ui_font_16)

#ifdef __cplusplus
}
#endif
