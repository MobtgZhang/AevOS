#include "widget.h"
#include "theme.h"
#include "font.h"
#include "../kernel/drivers/gpu_fb.h"
#include "../kernel/drivers/hid.h"
#include "../kernel/mm/slab.h"
#include "../lib/string.h"

widget_t *widget_create(widget_type_t type, int x, int y, int w, int h)
{
    widget_t *widget = (widget_t *)kmalloc(sizeof(widget_t));
    if (!widget)
        return NULL;

    memset(widget, 0, sizeof(widget_t));
    widget->type       = type;
    widget->bounds.x   = x;
    widget->bounds.y   = y;
    widget->bounds.w   = w;
    widget->bounds.h   = h;
    widget->visible    = true;
    widget->focused    = false;
    widget->fg_color   = COLOR_TEXT;
    widget->bg_color   = COLOR_BG;
    widget->text[0]    = '\0';
    widget->text_cursor = 0;
    widget->on_click   = NULL;
    widget->parent     = NULL;
    widget->child_count = 0;
    widget->scroll_offset = 0;
    widget->max_scroll = 0;

    static uint32_t next_id = 1;
    widget->id = next_id++;

    return widget;
}

void widget_destroy(widget_t *w)
{
    if (!w)
        return;

    for (int i = 0; i < w->child_count; i++)
        widget_destroy(w->children[i]);

    kfree(w);
}

void widget_set_text(widget_t *w, const char *text)
{
    if (!w || !text)
        return;
    strncpy(w->text, text, sizeof(w->text) - 1);
    w->text[sizeof(w->text) - 1] = '\0';
}

void widget_add_child(widget_t *parent, widget_t *child)
{
    if (!parent || !child)
        return;
    if (parent->child_count >= 32)
        return;

    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

/* ── Drawing primitives ── */

void draw_button(fb_ctx_t *fb, rect_t rect, const char *text,
                 bool is_hovered, bool is_pressed)
{
    uint32_t bg = COLOR_PANEL;
    if (is_pressed)
        bg = COLOR_ACCENT;
    else if (is_hovered)
        bg = color_brighten(COLOR_PANEL, 20);

    fb_draw_rect(rect.x, rect.y, rect.w, rect.h, bg);
    fb_draw_rect_outline(rect.x, rect.y, rect.w, rect.h,
                         is_hovered ? COLOR_ACCENT : COLOR_INPUT_BORDER, 1);

    if (text) {
        const font_t *fnt = font_get_default();
        int tw = font_measure_string(text, fnt);
        int tx = rect.x + (rect.w - tw) / 2;
        int ty = rect.y + (rect.h - fnt->height) / 2;
        font_draw_string(fb, tx, ty, text,
                         is_pressed ? COLOR_BG : COLOR_TEXT, 0, fnt);
    }
}

void draw_textbox(fb_ctx_t *fb, rect_t rect, const char *text,
                  int cursor_pos, bool is_focused)
{
    fb_draw_rect(rect.x, rect.y, rect.w, rect.h, COLOR_INPUT_BG);
    fb_draw_rect_outline(rect.x, rect.y, rect.w, rect.h,
                         is_focused ? COLOR_ACCENT : COLOR_INPUT_BORDER, 1);

    const font_t *fnt = font_get_default();
    int text_x = rect.x + PADDING;
    int text_y = rect.y + (rect.h - fnt->height) / 2;

    if (text && text[0]) {
        int max_chars = (rect.w - PADDING * 2) / fnt->width;
        int len = (int)strlen(text);
        int start = 0;
        if (cursor_pos > max_chars)
            start = cursor_pos - max_chars;

        for (int i = start; i < len && (i - start) < max_chars; i++) {
            font_draw_char(fb, text_x + (i - start) * fnt->width, text_y,
                           text[i], COLOR_TEXT, 0, fnt);
        }

        if (is_focused) {
            int cx = text_x + (cursor_pos - start) * fnt->width;
            fb_draw_rect(cx, text_y, 2, fnt->height, COLOR_ACCENT);
        }
    } else {
        if (is_focused) {
            fb_draw_rect(text_x, text_y, 2, fnt->height, COLOR_ACCENT);
        }
    }
}

void draw_scrollbar(fb_ctx_t *fb, int x, int y, int h,
                    int scroll_offset, int content_height, int view_height)
{
    (void)fb;
    if (content_height <= view_height)
        return;

    int bar_w = 6;
    fb_draw_rect(x, y, bar_w, h, COLOR_BG);

    int thumb_h = MAX(20, (int)((int64_t)view_height * h / content_height));
    int max_scroll = content_height - view_height;
    int thumb_y = y;
    if (max_scroll > 0)
        thumb_y = y + (int)((int64_t)scroll_offset * (h - thumb_h) / max_scroll);

    fb_draw_rect(x + 1, thumb_y, bar_w - 2, thumb_h, COLOR_SCROLLBAR);
}

void draw_panel(fb_ctx_t *fb, rect_t rect, const char *title,
                uint32_t bg_color)
{
    fb_draw_rect(rect.x, rect.y, rect.w, rect.h, bg_color);

    if (title && title[0]) {
        const font_t *fnt = font_get_default();
        int ty = rect.y + PADDING;
        font_draw_string(fb, rect.x + PADDING, ty, title,
                         COLOR_TEXT_DIM, 0, fnt);
        int line_y = ty + fnt->height + MARGIN;
        fb_draw_rect(rect.x, line_y, rect.w, 1, COLOR_DIVIDER);
    }
}

/* ── Widget rendering ── */

static void widget_draw_label(widget_t *w, fb_ctx_t *fb)
{
    const font_t *fnt = font_get_default();
    font_draw_string(fb, w->bounds.x, w->bounds.y, w->text,
                     w->fg_color, 0, fnt);
}

static void widget_draw_button(widget_t *w, fb_ctx_t *fb)
{
    draw_button(fb, w->bounds, w->text, false, false);
}

static void widget_draw_textbox(widget_t *w, fb_ctx_t *fb)
{
    draw_textbox(fb, w->bounds, w->text, w->text_cursor, w->focused);
}

static void widget_draw_panel(widget_t *w, fb_ctx_t *fb)
{
    draw_panel(fb, w->bounds, w->text, w->bg_color);
}

static void widget_draw_scrollview(widget_t *w, fb_ctx_t *fb)
{
    fb_draw_rect(w->bounds.x, w->bounds.y, w->bounds.w, w->bounds.h,
                 w->bg_color);

    for (int i = 0; i < w->child_count; i++) {
        if (w->children[i]->visible)
            widget_draw(w->children[i], fb);
    }

    if (w->max_scroll > 0)
        draw_scrollbar(fb, w->bounds.x + w->bounds.w - 8, w->bounds.y,
                       w->bounds.h, w->scroll_offset,
                       w->max_scroll + w->bounds.h, w->bounds.h);
}

static void widget_draw_list(widget_t *w, fb_ctx_t *fb)
{
    fb_draw_rect(w->bounds.x, w->bounds.y, w->bounds.w, w->bounds.h,
                 w->bg_color);

    for (int i = 0; i < w->child_count; i++) {
        if (w->children[i]->visible)
            widget_draw(w->children[i], fb);
    }
}

void widget_draw(widget_t *w, fb_ctx_t *fb)
{
    if (!w || !w->visible || !fb)
        return;

    switch (w->type) {
    case WIDGET_LABEL:      widget_draw_label(w, fb);      break;
    case WIDGET_BUTTON:     widget_draw_button(w, fb);     break;
    case WIDGET_TEXTBOX:    widget_draw_textbox(w, fb);    break;
    case WIDGET_SCROLLVIEW: widget_draw_scrollview(w, fb); break;
    case WIDGET_LIST:       widget_draw_list(w, fb);       break;
    case WIDGET_PANEL:      widget_draw_panel(w, fb);      break;
    }

    for (int i = 0; i < w->child_count; i++) {
        if (w->children[i]->visible)
            widget_draw(w->children[i], fb);
    }
}

/* ── Input handling ── */

static bool textbox_handle_key(widget_t *w, input_event_t *ev)
{
    if (ev->type != 1)
        return false;

    int len = (int)strlen(w->text);

    if (ev->keycode == KEY_BACKSPACE) {
        if (w->text_cursor > 0 && len > 0) {
            for (int i = w->text_cursor - 1; i < len; i++)
                w->text[i] = w->text[i + 1];
            w->text_cursor--;
        }
        return true;
    }

    if (ev->keycode == KEY_LEFT) {
        if (w->text_cursor > 0)
            w->text_cursor--;
        return true;
    }

    if (ev->keycode == KEY_RIGHT) {
        if (w->text_cursor < len)
            w->text_cursor++;
        return true;
    }

    if (ev->keycode == KEY_HOME) {
        w->text_cursor = 0;
        return true;
    }

    if (ev->keycode == KEY_END) {
        w->text_cursor = len;
        return true;
    }

    if (ev->keycode >= 32 && ev->keycode < 127 && len < 510) {
        for (int i = len; i >= w->text_cursor; i--)
            w->text[i + 1] = w->text[i];
        w->text[w->text_cursor] = (char)ev->keycode;
        w->text_cursor++;
        return true;
    }

    return false;
}

bool widget_handle_input(widget_t *w, input_event_t *ev)
{
    if (!w || !w->visible || !ev)
        return false;

    if (ev->type == 2) {
        if (rect_contains(&w->bounds, ev->mouse_x, ev->mouse_y)) {
            if (w->on_click)
                w->on_click(w);
            w->focused = true;
            return true;
        } else {
            w->focused = false;
            return false;
        }
    }

    if (w->focused && w->type == WIDGET_TEXTBOX)
        return textbox_handle_key(w, ev);

    for (int i = 0; i < w->child_count; i++) {
        if (widget_handle_input(w->children[i], ev))
            return true;
    }

    return false;
}
