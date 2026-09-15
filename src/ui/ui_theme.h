/**
 * @file ui_theme.h
 * @brief Common layout metrics, typography, and palette for ESP32-S3 Mini OS 240x320 Portrait UI
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
#define APP_HEADER_HEIGHT    28
#define DOCK_HEIGHT          52
#define APP_CONTENT_HEIGHT   (SCREEN_HEIGHT - STATUS_BAR_HEIGHT - APP_HEADER_HEIGHT) // 270 px
#define DESKTOP_GRID_HEIGHT  (SCREEN_HEIGHT - STATUS_BAR_HEIGHT - DOCK_HEIGHT)       // 246 px

// Touch accessibility & widget dimensions
#define MIN_TOUCH_SIZE       32
#define APP_ICON_BOX_SIZE    44   // 38-48px per requirement
#define APP_ICON_RADIUS      14   // iOS squircle radius
#define DOCK_ICON_BOX_SIZE   40
#define DOCK_ICON_RADIUS     12

// Animation timings (native LVGL 120-250ms)
#define ANIM_TIME_FAST_MS    140
#define ANIM_TIME_NORM_MS    200

// ==============================================================================
// 2. MODERN DARK PALETTE (TikTok / Sleek Glassmorphism Mobile OS)
// ==============================================================================
#define COLOR_OS_BG          0x0A0D14   // Deep Obsidian Black
#define COLOR_CARD_BG        0x141A26   // Elevated Dark Surface
#define COLOR_CARD_BORDER    0x232D3F   // Subtle Surface Border
#define COLOR_CARD_PRESSED   0x1E2638   // Active Tap State
#define COLOR_DOCK_BG        0x121722   // Translucent Floating Dock
#define COLOR_DOCK_BORDER    0x2B374E   // Subtle Dock Border
#define COLOR_HEADER_BG      0x0E1420   // App Window Navigation Header

// Vibrant Neon Accents
#define COLOR_ACCENT_CYAN    0x00F2FE   // Neon Cyan
#define COLOR_ACCENT_PURPLE  0xA855F7   // Modern Purple / Violet
#define COLOR_ACCENT_RED     0xFF3B5C   // TikTok Coral Red
#define COLOR_ACCENT_GREEN   0x10B981   // Emerald Green
#define COLOR_ACCENT_AMBER   0xF59E0B   // Warm Amber
#define COLOR_ACCENT_BLUE    0x3B82F6   // Vibrant Blue

// Typography Palette
#define COLOR_TEXT_WHITE     0xFFFFFF
#define COLOR_TEXT_SECONDARY 0xCBD5E1   // Slate-300
#define COLOR_TEXT_MUTED     0x64748B   // Slate-500
