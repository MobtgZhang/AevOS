#pragma once

#include <aevos/types.h>

/* ── Cursor-style dark color palette ── */

#define COLOR_BG            0xFF0D1117
#define COLOR_SIDEBAR       0xFF161B22
#define COLOR_PANEL         0xFF1C2128
#define COLOR_TEXT          0xFFE6EDF3
#define COLOR_TEXT_DIM      0xFF8B949E
#define COLOR_ACCENT        0xFF2188FF
#define COLOR_GREEN         0xFF3FB950
#define COLOR_RED           0xFFF85149
#define COLOR_YELLOW        0xFFD29922
#define COLOR_ORANGE        0xFFDB6D28
#define COLOR_PURPLE        0xFFBC8CFF
#define COLOR_AI_BUBBLE     0xFF1C2128
#define COLOR_USER_BUBBLE   0xFF0D419D
#define COLOR_INPUT_BG      0xFF0D1117
#define COLOR_INPUT_BORDER  0xFF30363D
#define COLOR_SCROLLBAR     0xFF484F58
#define COLOR_DIVIDER       0xFF21262D

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
