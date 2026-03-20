#pragma once

#include <aevos/types.h>
#include <aevos/boot_info.h>

#define FB_FONT_W 8
#define FB_FONT_H 16

typedef struct fb_ctx {
    uint32_t *pixels;
    uint32_t *back_buffer;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    uint32_t  bpp;
    uint64_t  phys_base;
    bool      double_buffered;
} fb_ctx_t;

void      fb_init(framebuffer_t *boot_fb);
void      fb_clear(uint32_t color);
void      fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void      fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void      fb_draw_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                               uint32_t color, uint32_t thickness);
void      fb_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color);
void      fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void      fb_draw_string(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg);
void      fb_scroll(uint32_t region_y, uint32_t region_h, uint32_t lines);
void      fb_blit(uint32_t dst_x, uint32_t dst_y,
                  const uint32_t *src, uint32_t src_w, uint32_t src_h);
void      fb_draw_cursor(int32_t x, int32_t y, uint32_t color);
void      fb_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                               uint32_t radius, uint32_t color);
void      fb_draw_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color);
void      fb_alpha_blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                              uint32_t color, uint8_t alpha);
void      fb_swap_buffers(void);
fb_ctx_t *fb_get_ctx(void);
