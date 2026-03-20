#pragma once

#include <aevos/types.h>

typedef struct fb_ctx fb_ctx_t;
typedef struct input_event input_event_t;

typedef enum {
    WIDGET_LABEL,
    WIDGET_BUTTON,
    WIDGET_TEXTBOX,
    WIDGET_SCROLLVIEW,
    WIDGET_LIST,
    WIDGET_PANEL
} widget_type_t;

typedef struct {
    int x, y, w, h;
} rect_t;

static inline bool rect_contains(const rect_t *r, int px, int py)
{
    return px >= r->x && px < r->x + r->w &&
           py >= r->y && py < r->y + r->h;
}

typedef struct widget widget_t;

typedef void (*widget_click_fn)(widget_t *self);

struct widget {
    widget_type_t   type;
    rect_t          bounds;
    bool            visible;
    bool            focused;
    uint32_t        id;
    char            text[512];
    int             text_cursor;
    widget_click_fn on_click;
    widget_t       *parent;
    widget_t       *children[32];
    int             child_count;
    int             scroll_offset;
    int             max_scroll;
    uint32_t        fg_color;
    uint32_t        bg_color;
};

widget_t *widget_create(widget_type_t type, int x, int y, int w, int h);
void      widget_destroy(widget_t *w);
void      widget_draw(widget_t *w, fb_ctx_t *fb);
bool      widget_handle_input(widget_t *w, input_event_t *ev);
void      widget_set_text(widget_t *w, const char *text);
void      widget_add_child(widget_t *parent, widget_t *child);

void draw_button(fb_ctx_t *fb, rect_t rect, const char *text,
                 bool is_hovered, bool is_pressed);
void draw_textbox(fb_ctx_t *fb, rect_t rect, const char *text,
                  int cursor_pos, bool is_focused);
void draw_scrollbar(fb_ctx_t *fb, int x, int y, int h,
                    int scroll_offset, int content_height, int view_height);
void draw_panel(fb_ctx_t *fb, rect_t rect, const char *title,
                uint32_t bg_color);
