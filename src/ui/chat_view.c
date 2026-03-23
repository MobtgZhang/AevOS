#include "chat_view.h"
#include "theme.h"
#include "font.h"
#include "../kernel/drivers/gpu_fb.h"
#include "../kernel/drivers/hid.h"
#include "../kernel/locale.h"
#include "../kernel/klog.h"
#include "../lib/string.h"

void chat_view_init(chat_view_t *cv, rect_t bounds)
{
    if (!cv)
        return;

    memset(cv, 0, sizeof(chat_view_t));
    cv->view_rect     = bounds;
    cv->max_messages  = CHAT_MAX_MESSAGES;
    cv->message_count = 0;
    cv->scroll_offset = 0;
    cv->input_cursor  = 0;
    cv->is_ai_typing  = false;
    cv->input_text[0] = '\0';
}

void chat_view_add_message(chat_view_t *cv, chat_role_t role,
                           const char *text)
{
    if (!cv || !text)
        return;

    if (cv->message_count >= cv->max_messages) {
        for (int i = 0; i < cv->message_count - 1; i++)
            cv->messages[i] = cv->messages[i + 1];
        cv->message_count--;
    }

    chat_message_t *msg = &cv->messages[cv->message_count];
    msg->role = role;
    strncpy(msg->text, text, CHAT_MSG_TEXT_SIZE - 1);
    msg->text[CHAT_MSG_TEXT_SIZE - 1] = '\0';
    msg->timestamp    = 0;
    msg->is_streaming = false;
    cv->message_count++;

    int total_h = 0;
    const font_t *fnt = font_get_default();
    int bubble_w = cv->view_rect.w - PADDING * 4 - 40;
    for (int i = 0; i < cv->message_count; i++) {
        int h = font_measure_string_wrapped(cv->messages[i].text,
                                            bubble_w, fnt);
        total_h += h + PADDING * 3 + fnt->height;
    }

    int view_h = cv->view_rect.h - INPUT_HEIGHT - PADDING;
    if (total_h > view_h)
        cv->scroll_offset = total_h - view_h;
    else
        cv->scroll_offset = 0;
}

void chat_view_append_stream_chunk(chat_view_t *cv, const char *chunk)
{
    if (!cv || !chunk || !*chunk || cv->message_count == 0)
        return;
    chat_message_t *last = &cv->messages[cv->message_count - 1];
    size_t cur = strlen(last->text);
    size_t cl = strlen(chunk);
    if (cur + cl >= CHAT_MSG_TEXT_SIZE)
        cl = CHAT_MSG_TEXT_SIZE - 1 - cur;
    if (cl == 0)
        return;
    memcpy(last->text + cur, chunk, cl);
    last->text[cur + cl] = '\0';
    last->is_streaming = true;

    int total_h = 0;
    const font_t *fnt = font_get_default();
    int bubble_w = cv->view_rect.w - PADDING * 4 - 40;
    for (int i = 0; i < cv->message_count; i++) {
        int h = font_measure_string_wrapped(cv->messages[i].text,
                                            bubble_w, fnt);
        total_h += h + PADDING * 3 + fnt->height;
    }
    int view_h = cv->view_rect.h - INPUT_HEIGHT - PADDING;
    if (total_h > view_h)
        cv->scroll_offset = total_h - view_h;
}

void chat_view_update_streaming(chat_view_t *cv, const char *text)
{
    if (!cv || !text || cv->message_count == 0)
        return;

    chat_message_t *last = &cv->messages[cv->message_count - 1];
    strncpy(last->text, text, CHAT_MSG_TEXT_SIZE - 1);
    last->text[CHAT_MSG_TEXT_SIZE - 1] = '\0';
    last->is_streaming = true;

    int total_h = 0;
    const font_t *fnt = font_get_default();
    int bubble_w = cv->view_rect.w - PADDING * 4 - 40;
    for (int i = 0; i < cv->message_count; i++) {
        int h = font_measure_string_wrapped(cv->messages[i].text,
                                            bubble_w, fnt);
        total_h += h + PADDING * 3 + fnt->height;
    }

    int view_h = cv->view_rect.h - INPUT_HEIGHT - PADDING;
    if (total_h > view_h)
        cv->scroll_offset = total_h - view_h;
}

static void render_message_bubble(fb_ctx_t *fb, chat_message_t *msg,
                                  int x, int y, int max_w,
                                  const font_t *fnt)
{
    bool is_user = (msg->role == CHAT_ROLE_USER);
    uint32_t bubble_bg = is_user ? COLOR_USER_BUBBLE : COLOR_AI_BUBBLE;
    uint32_t text_color = COLOR_TEXT;
    int bubble_pad = PADDING;
    int text_w = max_w - bubble_pad * 2;

    int text_h = font_measure_string_wrapped(msg->text, text_w, fnt);
    int bubble_h = text_h + bubble_pad * 2;
    int label_h = fnt->height + MARGIN;

    int bubble_x;
    if (is_user)
        bubble_x = x + 40;
    else
        bubble_x = x;

    const char *label = is_user ? "You" : "AevOS";
    uint32_t label_color = is_user ? COLOR_ACCENT : COLOR_GREEN;
    font_draw_string(fb, bubble_x + bubble_pad, y, label,
                     label_color, 0, fnt);

    int bubble_y = y + label_h;
    fb_draw_rect(bubble_x, bubble_y, max_w - 40, bubble_h, bubble_bg);

    fb_draw_rect(bubble_x, bubble_y, max_w - 40, 1,
                 color_brighten(bubble_bg, 15));

    font_draw_string_wrap(fb, bubble_x + bubble_pad,
                          bubble_y + bubble_pad,
                          text_w, msg->text,
                          text_color, 0, fnt);

    if (msg->is_streaming) {
        int last_line_len = 0;
        const char *p = msg->text;
        while (*p) {
            if (*p == '\n')
                last_line_len = 0;
            else
                last_line_len++;
            p++;
        }
        int chars_per_line = text_w / fnt->width;
        if (chars_per_line <= 0)
            chars_per_line = 1;

        int lines = font_draw_string_wrap(NULL, 0, 0, text_w,
                                          msg->text, 0, 0, fnt);
        int cursor_x = bubble_x + bubble_pad +
                        (last_line_len % chars_per_line) * fnt->width;
        int cursor_y = bubble_y + bubble_pad +
                        (lines - 1) * fnt->height;
        fb_draw_rect(cursor_x, cursor_y, fnt->width, fnt->height,
                     COLOR_ACCENT);
    }
}

void chat_view_render(chat_view_t *cv, fb_ctx_t *fb)
{
    if (!cv || !fb)
        return;

    fb_draw_rect(cv->view_rect.x, cv->view_rect.y,
                 cv->view_rect.w, cv->view_rect.h, COLOR_BG);

    const font_t *fnt = font_get_default();
    int msg_area_h = cv->view_rect.h - INPUT_HEIGHT - PADDING;
    int bubble_w = cv->view_rect.w - PADDING * 2;

    int y_offset = cv->view_rect.y + PADDING - cv->scroll_offset;

    for (int i = 0; i < cv->message_count; i++) {
        chat_message_t *msg = &cv->messages[i];
        int text_w = bubble_w - PADDING * 2 - 40;
        int text_h = font_measure_string_wrapped(msg->text, text_w, fnt);
        int item_h = text_h + PADDING * 3 + fnt->height;

        int bottom = y_offset + item_h;
        int area_top = cv->view_rect.y;
        int area_bot = cv->view_rect.y + msg_area_h;

        if (bottom > area_top && y_offset < area_bot) {
            render_message_bubble(fb, msg,
                                  cv->view_rect.x + PADDING,
                                  y_offset, bubble_w, fnt);
        }
        y_offset += item_h;
    }

    if (cv->is_ai_typing && cv->message_count > 0) {
        chat_message_t *last = &cv->messages[cv->message_count - 1];
        if (!last->is_streaming) {
            const char *dots = "AevOS is thinking...";
            font_draw_string(fb, cv->view_rect.x + PADDING * 2,
                             y_offset + PADDING,
                             dots, COLOR_TEXT_DIM, 0, fnt);
        }
    }

    int total_content = y_offset - (cv->view_rect.y + PADDING - cv->scroll_offset);
    if (total_content > msg_area_h) {
        draw_scrollbar(fb,
                       cv->view_rect.x + cv->view_rect.w - 8,
                       cv->view_rect.y,
                       msg_area_h,
                       cv->scroll_offset,
                       total_content,
                       msg_area_h);
    }

    chat_view_render_input_box(cv, fb);
}

void chat_view_render_input_box(chat_view_t *cv, fb_ctx_t *fb)
{
    if (!cv || !fb)
        return;

    int input_y = cv->view_rect.y + cv->view_rect.h - INPUT_HEIGHT;
    rect_t input_rect = {
        .x = cv->view_rect.x + PADDING,
        .y = input_y,
        .w = cv->view_rect.w - PADDING * 2,
        .h = INPUT_HEIGHT - PADDING
    };

    fb_draw_rect(input_rect.x, input_rect.y,
                 input_rect.w, input_rect.h, COLOR_INPUT_BG);
    fb_draw_rect_outline(input_rect.x, input_rect.y,
                         input_rect.w, input_rect.h, COLOR_INPUT_BORDER, 1);

    const font_t *fnt = font_get_default();
    int text_x = input_rect.x + PADDING;
    int text_y = input_rect.y + (input_rect.h - fnt->height) / 2;

    if (cv->input_text[0] == '\0') {
        font_draw_string(fb, text_x, text_y,
                         "Type a message... (Enter to send)",
                         COLOR_TEXT_DIM, 0, fnt);
    } else {
        int max_chars = (input_rect.w - PADDING * 2) / fnt->width;
        int len = (int)strlen(cv->input_text);
        int start = 0;
        if (cv->input_cursor > max_chars)
            start = cv->input_cursor - max_chars;

        for (int i = start; i < len && (i - start) < max_chars; i++) {
            font_draw_char(fb, text_x + (i - start) * fnt->width,
                           text_y, cv->input_text[i],
                           COLOR_TEXT, 0, fnt);
        }
    }

    int cx = text_x + (cv->input_cursor) * fnt->width;
    fb_draw_rect(cx, text_y, 2, fnt->height, COLOR_ACCENT);
}

bool chat_view_handle_input(chat_view_t *cv, input_event_t *ev)
{
    if (!cv || !ev)
        return false;

    if (ev->type == 3) {
        int msg_area_h = cv->view_rect.h - INPUT_HEIGHT - PADDING;
        int total_h = 0;
        const font_t *fnt = font_get_default();
        int bubble_w = cv->view_rect.w - PADDING * 4 - 40;
        for (int i = 0; i < cv->message_count; i++) {
            int h = font_measure_string_wrapped(cv->messages[i].text,
                                                bubble_w, fnt);
            total_h += h + PADDING * 3 + fnt->height;
        }

        int max_scroll = total_h - msg_area_h;
        if (max_scroll < 0) max_scroll = 0;

        cv->scroll_offset -= ev->mouse_y * 3;
        if (cv->scroll_offset < 0)
            cv->scroll_offset = 0;
        if (cv->scroll_offset > max_scroll)
            cv->scroll_offset = max_scroll;
        return true;
    }

    if (ev->type != 1)
        return false;

    int len = (int)strlen(cv->input_text);

    if (ev->keycode == KEY_ENTER) {
        if (len > 0) {
            chat_view_add_message(cv, CHAT_ROLE_USER, cv->input_text);
            cv->input_text[0] = '\0';
            cv->input_cursor  = 0;
            return true;
        }
        return false;
    }

    if (ev->keycode == KEY_BACKSPACE) {
        if (cv->input_cursor > 0 && len > 0) {
            for (int i = cv->input_cursor - 1; i < len; i++)
                cv->input_text[i] = cv->input_text[i + 1];
            cv->input_cursor--;
            return true;
        }
        return false;
    }

    if (ev->keycode == KEY_LEFT) {
        if (cv->input_cursor > 0)
            cv->input_cursor--;
        return true;
    }
    if (ev->keycode == KEY_RIGHT) {
        if (cv->input_cursor < len)
            cv->input_cursor++;
        return true;
    }
    if (ev->keycode == KEY_HOME) {
        cv->input_cursor = 0;
        return true;
    }
    if (ev->keycode == KEY_END) {
        cv->input_cursor = len;
        return true;
    }

    if (ev->keycode == KEY_UP) {
        cv->scroll_offset -= FONT_HEIGHT * 2;
        if (cv->scroll_offset < 0)
            cv->scroll_offset = 0;
        return true;
    }
    if (ev->keycode == KEY_DOWN) {
        cv->scroll_offset += FONT_HEIGHT * 2;
        return true;
    }

    {
        char ch = keycode_to_char(ev->keycode, ev->modifiers);
        if (ch >= 32 && ch < 127 && len < CHAT_INPUT_SIZE - 1) {
            for (int i = len; i >= cv->input_cursor; i--)
                cv->input_text[i + 1] = cv->input_text[i];
            cv->input_text[cv->input_cursor] = ch;
            cv->input_cursor++;
            return true;
        }
    }

    return false;
}
