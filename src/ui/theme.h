#pragma once

#include <aevos/types.h>

/* ── Aero-style glass desktop palette（磨砂蓝玻璃 + 高光标题栏） ── */

#define COLOR_AERO_HIGHLIGHT  0xFF9ECFFF
#define COLOR_AERO_RIM        0xFF6FA8DC

#define COLOR_BG            0xFF1E3A5F
#define COLOR_SIDEBAR       0xFF243B5A
#define COLOR_PANEL         0xFF2E4A6E
#define COLOR_TEXT          0xFFF0F7FF
#define COLOR_TEXT_DIM      0xFFB8C9DC
#define COLOR_ACCENT        0xFF4DA3FF
#define COLOR_GREEN         0xFF3FB950
#define COLOR_RED           0xFFF85149
#define COLOR_YELLOW        0xFFD29922
#define COLOR_ORANGE        0xFFDB6D28
#define COLOR_PURPLE        0xFFBC8CFF
#define COLOR_AI_BUBBLE     0xFF2E4A6E
#define COLOR_USER_BUBBLE   0xFF1B5EB8
#define COLOR_INPUT_BG      0xFF1A3048
#define COLOR_INPUT_BORDER  0xFF5B7FA6
#define COLOR_SCROLLBAR     0xFF6B8CAF
#define COLOR_DIVIDER       0xFF3D5A7A

/* ── Font metrics ── */

#define FONT_WIDTH          8
#define FONT_HEIGHT         16

/* ── Layout constants ── */

#define SIDEBAR_WIDTH       220
#define PANEL_WIDTH         250
#define TITLEBAR_HEIGHT     32
#define STATUSBAR_HEIGHT    24
#define INPUT_HEIGHT        48
#define PADDING             8
#define MARGIN              4

/* ── Derived color helpers ── */

static inline uint32_t color_alpha_blend(uint32_t fg, uint32_t bg, uint8_t alpha)
{
    uint8_t fr = (fg >> 16) & 0xFF, fg_g = (fg >> 8) & 0xFF, fb_ = fg & 0xFF;
    uint8_t br = (bg >> 16) & 0xFF, bg_g = (bg >> 8) & 0xFF, bb  = bg & 0xFF;
    uint8_t r = (uint8_t)(((uint16_t)fr * alpha + (uint16_t)br * (255 - alpha)) / 255);
    uint8_t g = (uint8_t)(((uint16_t)fg_g * alpha + (uint16_t)bg_g * (255 - alpha)) / 255);
    uint8_t b = (uint8_t)(((uint16_t)fb_ * alpha + (uint16_t)bb * (255 - alpha)) / 255);
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t color_brighten(uint32_t color, int amount)
{
    int r = MIN(255, (int)((color >> 16) & 0xFF) + amount);
    int g = MIN(255, (int)((color >>  8) & 0xFF) + amount);
    int b = MIN(255, (int)((color      ) & 0xFF) + amount);
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint32_t color_darken(uint32_t color, int amount)
{
    int r = MAX(0, (int)((color >> 16) & 0xFF) - amount);
    int g = MAX(0, (int)((color >>  8) & 0xFF) - amount);
    int b = MAX(0, (int)((color      ) & 0xFF) - amount);
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
