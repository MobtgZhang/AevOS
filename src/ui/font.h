#pragma once

#include <aevos/types.h>

typedef struct fb_ctx fb_ctx_t;

typedef struct {
    uint8_t        width;
    uint8_t        height;
    const uint8_t *glyph_data;
    uint8_t        first_char;
    uint8_t        last_char;
} font_t;

void    font_init(void);
font_t *font_get_default(void);

void font_draw_char(fb_ctx_t *fb, int x, int y, char c,
                    uint32_t fg, uint32_t bg, const font_t *font);
void font_draw_string(fb_ctx_t *fb, int x, int y, const char *text,
                      uint32_t fg, uint32_t bg, const font_t *font);
int  font_draw_string_wrap(fb_ctx_t *fb, int x, int y, int max_width,
                           const char *text, uint32_t fg, uint32_t bg,
                           const font_t *font);
int  font_measure_string(const char *text, const font_t *font);
int  font_measure_string_wrapped(const char *text, int max_width,
                                 const font_t *font);
