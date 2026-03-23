#pragma once

#include <aevos/types.h>
#include "widget.h"

typedef struct fb_ctx fb_ctx_t;
typedef struct input_event input_event_t;

#define CHAT_MAX_MESSAGES   256
#define CHAT_MSG_TEXT_SIZE  4096
#define CHAT_INPUT_SIZE    1024

typedef enum {
    CHAT_ROLE_USER,
    CHAT_ROLE_ASSISTANT,
    CHAT_ROLE_SYSTEM
} chat_role_t;

typedef struct {
    chat_role_t role;
    char        text[CHAT_MSG_TEXT_SIZE];
    uint64_t    timestamp;
    bool        is_streaming;
} chat_message_t;

typedef struct {
    chat_message_t messages[CHAT_MAX_MESSAGES];
    int            message_count;
    int            max_messages;
    int            scroll_offset;
    rect_t         view_rect;
    char           input_text[CHAT_INPUT_SIZE];
    int            input_cursor;
    bool           is_ai_typing;
} chat_view_t;

void chat_view_init(chat_view_t *cv, rect_t bounds);
void chat_view_add_message(chat_view_t *cv, chat_role_t role,
                           const char *text);
void chat_view_update_streaming(chat_view_t *cv, const char *text);
/* Append a token/chunk to the last assistant message (streaming UI). */
void chat_view_append_stream_chunk(chat_view_t *cv, const char *chunk);
void chat_view_render(chat_view_t *cv, fb_ctx_t *fb);
bool chat_view_handle_input(chat_view_t *cv, input_event_t *ev);
void chat_view_render_input_box(chat_view_t *cv, fb_ctx_t *fb);
