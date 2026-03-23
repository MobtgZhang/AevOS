#pragma once

#include <aevos/types.h>
#include "widget.h"

typedef struct fb_ctx fb_ctx_t;
typedef struct input_event input_event_t;

#define TERM_MAX_LINES     512
#define TERM_LINE_LEN      256
#define TERM_INPUT_LEN     512
#define TERM_HISTORY_COUNT 64
#define TERM_HISTORY_LEN   256
#define TERM_MAX_ENV       16
#define TERM_ENV_KEY_LEN   40
#define TERM_ENV_VAL_LEN   128
#define TERM_CWD_LEN       256

typedef struct {
    char key[TERM_ENV_KEY_LEN];
    char val[TERM_ENV_VAL_LEN];
} terminal_env_entry_t;

typedef struct {
    char     text[TERM_LINE_LEN];
    uint32_t fg_colors[TERM_LINE_LEN];
    int      len;
} terminal_line_t;

typedef struct {
    terminal_line_t lines[TERM_MAX_LINES];
    int             line_count;
    int             max_lines;
    int             scroll_offset;
    rect_t          bounds;
    char            input_buf[TERM_INPUT_LEN];
    int             input_len;
    int             cursor_pos;
    char            command_history[TERM_HISTORY_COUNT][TERM_HISTORY_LEN];
    int             history_count;
    int             history_index;
    uint32_t        blink_counter;
    char            cwd[TERM_CWD_LEN];
    terminal_env_entry_t env[TERM_MAX_ENV];
    int             env_count;
    bool            scroll_follow_bottom;
} terminal_t;

void terminal_init(terminal_t *term, rect_t bounds);
void terminal_render(terminal_t *term, fb_ctx_t *fb);
bool terminal_handle_input(terminal_t *term, input_event_t *ev);
/* Mouse wheel / notched scroll when pointer is over terminal (dy>0 = scroll toward older lines). */
bool terminal_handle_mouse_scroll(terminal_t *term, int32_t dy);
void terminal_execute_command(terminal_t *term, const char *cmd);
void terminal_print(terminal_t *term, const char *text);
void terminal_printf(terminal_t *term, const char *fmt, ...);
