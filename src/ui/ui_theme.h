/**
 * @file ui_theme.h
 * @brief Common layout metrics, typography, and palette for the responsive Mini OS UI.
 */

#pragma once

#include <lvgl.h>
#include "board_config.h"
#include "fonts/ui_fonts.h"

// ==============================================================================
// 1. SCREEN DIMENSIONS & LAYOUT METRICS
// ==============================================================================
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH         BOARD_LCD_WIDTH
#endif

#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT        BOARD_LCD_HEIGHT
#endif

#define STATUS_BAR_HEIGHT    22
#define APP_HEADER_HEIGHT    44
#define DOCK_HEIGHT          66
#define HOME_GESTURE_HEIGHT  16
#define APP_CONTENT_HEIGHT   (SCREEN_HEIGHT - STATUS_BAR_HEIGHT - APP_HEADER_HEIGHT - HOME_GESTURE_HEIGHT)
#define DESKTOP_GRID_HEIGHT  (SCREEN_HEIGHT - STATUS_BAR_HEIGHT - DOCK_HEIGHT)

// Touch accessibility & widget dimensions
#define MIN_TOUCH_SIZE       44
#define APP_ICON_BOX_SIZE    44
#define APP_ICON_RADIUS      14   // Modern continuous squircle radius
#define DOCK_ICON_BOX_SIZE   68
#define DOCK_ICON_RADIUS     12

// Animation timings (native LVGL 120-250ms)
#define ANIM_TIME_FAST_MS    140
#define ANIM_TIME_NORM_MS    200

// ==============================================================================
// 2. iPhone prototype dark palette, adapted to the embedded display.
// ==============================================================================
#define COLOR_OS_BG          0x121319
#define COLOR_CARD_BG        0x202129
#define COLOR_CARD_BORDER    0x363742
#define COLOR_CARD_PRESSED   0x373844
#define COLOR_DOCK_BG        0x202129
#define COLOR_DOCK_BORDER    0x363742
#define COLOR_HEADER_BG      0x121319

// Legacy names retained for app compatibility; no color/byte-order changes.
#define COLOR_ACCENT_CYAN    0x8CB5FF
#define COLOR_ACCENT_PURPLE  0xC1B1F8
#define COLOR_ACCENT_RED     0xFFA6AE
#define COLOR_ACCENT_GREEN   0x8BDBC0
#define COLOR_ACCENT_AMBER   0xF4CB82
#define COLOR_ACCENT_BLUE    0x8CB5FF

// Typography Palette
#define COLOR_TEXT_WHITE     0xF7F7FB
#define COLOR_TEXT_SECONDARY 0xB0B1C1
#define COLOR_TEXT_MUTED     0xB0B1C1
