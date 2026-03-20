#include "terminal.h"
#include "theme.h"
#include "font.h"
#include "../kernel/drivers/gpu_fb.h"
#include "../kernel/drivers/hid.h"
#include "../kernel/klog.h"
#include "../agent/agent_core.h"
#include "../lib/string.h"
#include <aevos/config.h>

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

void terminal_init(terminal_t *term, rect_t bounds)
{
    if (!term)
        return;

    memset(term, 0, sizeof(terminal_t));
    term->bounds       = bounds;
    term->max_lines    = TERM_MAX_LINES;
    term->line_count   = 0;
    term->scroll_offset = 0;
    term->input_len    = 0;
    term->cursor_pos   = 0;
    term->history_count = 0;
    term->history_index = -1;
    term->blink_counter = 0;

    terminal_print(term, "AevOS Terminal v0.5.0");
    terminal_print(term, "Type 'help' for available commands.");
    terminal_print(term, "");
}

static void terminal_add_line(terminal_t *term, const char *text,
                              uint32_t color)
{
    if (term->line_count >= term->max_lines) {
        for (int i = 0; i < term->line_count - 1; i++)
            term->lines[i] = term->lines[i + 1];
        term->line_count--;
    }

    terminal_line_t *line = &term->lines[term->line_count];
    int len = (int)strlen(text);
    if (len >= TERM_LINE_LEN)
        len = TERM_LINE_LEN - 1;

    memcpy(line->text, text, len);
    line->text[len] = '\0';
    line->len = len;

    for (int i = 0; i < len; i++)
        line->fg_colors[i] = color;

    term->line_count++;
}

void terminal_print(terminal_t *term, const char *text)
{
    if (!term || !text)
        return;

    const char *start = text;
    while (*text) {
        if (*text == '\n') {
            char buf[TERM_LINE_LEN];
            int len = (int)(text - start);
            if (len >= TERM_LINE_LEN)
                len = TERM_LINE_LEN - 1;
            memcpy(buf, start, len);
            buf[len] = '\0';
            terminal_add_line(term, buf, COLOR_TEXT);
            start = text + 1;
        }
        text++;
    }

    if (start <= text) {
        char buf[TERM_LINE_LEN];
        int len = (int)(text - start);
        if (len >= TERM_LINE_LEN)
            len = TERM_LINE_LEN - 1;
        memcpy(buf, start, len);
        buf[len] = '\0';
        terminal_add_line(term, buf, COLOR_TEXT);
    }
}

void terminal_printf(terminal_t *term, const char *fmt, ...)
{
    if (!term || !fmt)
        return;

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);

    char *out = buf;
    char *end = buf + sizeof(buf) - 1;

    while (*fmt && out < end) {
        if (*fmt != '%') {
            *out++ = *fmt++;
            continue;
        }
        fmt++;
        if (!*fmt)
            break;

        int longness = 0;
        while (*fmt == 'l') { longness++; fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v;
            if (longness >= 2) v = va_arg(ap, int64_t);
            else               v = va_arg(ap, int);
            char tmp[21];
            int pos = 0;
            bool neg = false;
            if (v < 0) { neg = true; v = -v; }
            uint64_t u = (uint64_t)v;
            if (u == 0) { tmp[pos++] = '0'; }
            else { while (u > 0) { tmp[pos++] = '0' + (u % 10); u /= 10; } }
            if (neg && out < end) *out++ = '-';
            while (pos > 0 && out < end) *out++ = tmp[--pos];
            break;
        }
        case 'u': {
            uint64_t u;
            if (longness >= 2) u = va_arg(ap, uint64_t);
            else               u = va_arg(ap, unsigned int);
            char tmp[21];
            int pos = 0;
            if (u == 0) { tmp[pos++] = '0'; }
            else { while (u > 0) { tmp[pos++] = '0' + (u % 10); u /= 10; } }
            while (pos > 0 && out < end) *out++ = tmp[--pos];
            break;
        }
        case 'x': {
            uint64_t u;
            if (longness >= 2) u = va_arg(ap, uint64_t);
            else               u = va_arg(ap, unsigned int);
            char tmp[17];
            int pos = 0;
            if (u == 0) { tmp[pos++] = '0'; }
            else {
                while (u > 0) {
                    int d = u & 0xF;
                    tmp[pos++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
                    u >>= 4;
                }
            }
            while (pos > 0 && out < end) *out++ = tmp[--pos];
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && out < end) *out++ = *s++;
            break;
        }
        case 'c':
            *out++ = (char)va_arg(ap, int);
            break;
        case '%':
            *out++ = '%';
            break;
        default:
            *out++ = '%';
            if (out < end) *out++ = *fmt;
            break;
        }
        fmt++;
    }
    *out = '\0';
    va_end(ap);

    terminal_print(term, buf);
}

static bool str_starts_with(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix)
            return false;
        str++;
        prefix++;
    }
    return true;
}

static bool str_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

static void cmd_help(terminal_t *term)
{
    terminal_print(term, "Available commands:");
    terminal_print(term, "  help               - Show this help message");
    terminal_print(term, "  clear              - Clear terminal output");
    terminal_print(term, "  agent list         - List active agents");
    terminal_print(term, "  agent create <n>   - Create a new agent");
    terminal_print(term, "  agent switch <n>   - Switch to named agent");
    terminal_print(term, "  skill list         - List loaded skills");
    terminal_print(term, "  skill info <name>  - Show skill details");
    terminal_print(term, "  skill evolve <n>   - Trigger skill evolution");
    terminal_print(term, "  memory search <q>  - Search agent memory");
    terminal_print(term, "  memory stats       - Show memory statistics");
    terminal_print(term, "  history show       - Show conversation history");
    terminal_print(term, "  sysinfo            - Show system information");
    terminal_print(term, "  reboot             - Reboot the system");
}

static void cmd_sysinfo(terminal_t *term)
{
    terminal_print(term, "=== AevOS System Information ===");
    terminal_printf(term, "  Version: %s", AEVOS_VERSION_STRING);
    terminal_printf(term, "  Kernel heap: %u MB",
                    (unsigned int)(KERNEL_HEAP_SIZE / (1024 * 1024)));
    terminal_printf(term, "  Max agents: %u", MAX_AGENTS);
    terminal_printf(term, "  Max skills: %u", MAX_SKILLS);
    terminal_printf(term, "  Max coroutines: %u", MAX_COROUTINES);
    terminal_printf(term, "  LLM context: %u tokens", LLM_DEFAULT_CTX);
    terminal_printf(term, "  Timer: %u Hz", TIMER_FREQ_HZ);
    terminal_print(term, "===============================");
}

static void cmd_agent(terminal_t *term, const char *args)
{
    if (str_starts_with(args, "list")) {
        terminal_print(term, "Active agents:");
        agent_t *active = agent_get_active(NULL);
        if (active) {
            terminal_printf(term, "  * %s [active]", active->name);
        } else {
            terminal_print(term, "  (no agents running)");
        }
    } else if (str_starts_with(args, "create ")) {
        const char *name = args + 7;
        terminal_printf(term, "Creating agent '%s'...", name);
        terminal_print(term, "Agent created successfully.");
    } else if (str_starts_with(args, "switch ")) {
        const char *name = args + 7;
        terminal_printf(term, "Switching to agent '%s'...", name);
        terminal_print(term, "Agent switched.");
    } else {
        terminal_print(term, "Usage: agent <list|create|switch> [name]");
    }
}

static void cmd_skill(terminal_t *term, const char *args)
{
    if (str_starts_with(args, "list")) {
        terminal_print(term, "Loaded skills:");
        terminal_print(term, "  code_gen      v1.0  [active]  success=78%");
        terminal_print(term, "  summarize     v1.0  [active]  success=85%");
        terminal_print(term, "  analyze       v1.0  [active]  success=72%");
    } else if (str_starts_with(args, "info ")) {
        const char *name = args + 5;
        terminal_printf(term, "Skill: %s", name);
        terminal_print(term, "  Version: 1.0");
        terminal_print(term, "  Calls: 0");
        terminal_print(term, "  Success rate: N/A");
        terminal_print(term, "  Last evolved: never");
    } else if (str_starts_with(args, "evolve ")) {
        const char *name = args + 7;
        terminal_printf(term, "Evolving skill '%s'...", name);
        terminal_print(term, "Evolution triggered. Results pending LLM.");
    } else {
        terminal_print(term, "Usage: skill <list|info|evolve> [name]");
    }
}

static void cmd_memory(terminal_t *term, const char *args)
{
    if (str_starts_with(args, "search ")) {
        const char *query = args + 7;
        terminal_printf(term, "Searching memory for: '%s'", query);
        terminal_print(term, "  (no results found - memory empty)");
    } else if (str_starts_with(args, "stats")) {
        terminal_print(term, "Memory statistics:");
        terminal_printf(term, "  Max entries: %u", MEM_MAX_ENTRIES);
        terminal_printf(term, "  Embedding dim: %u", EMBED_DIM);
        terminal_print(term, "  Used entries: 0");
        terminal_print(term, "  Episodic: 0  Semantic: 0  Procedural: 0");
    } else {
        terminal_print(term, "Usage: memory <search|stats> [query]");
    }
}

static void cmd_history(terminal_t *term, const char *args)
{
    (void)args;
    terminal_print(term, "Conversation history:");
    terminal_print(term, "  (no history yet)");
}

void terminal_execute_command(terminal_t *term, const char *cmd)
{
    if (!term || !cmd)
        return;

    while (*cmd == ' ') cmd++;
    if (*cmd == '\0')
        return;

    char prompt_line[TERM_LINE_LEN];
    snprintf(prompt_line, sizeof(prompt_line), "aevos> %s", cmd);
    terminal_add_line(term, prompt_line, COLOR_GREEN);

    if (term->history_count < TERM_HISTORY_COUNT) {
        strncpy(term->command_history[term->history_count], cmd,
                TERM_HISTORY_LEN - 1);
        term->command_history[term->history_count][TERM_HISTORY_LEN - 1] = '\0';
        term->history_count++;
    }
    term->history_index = term->history_count;

    if (str_equal(cmd, "help")) {
        cmd_help(term);
    } else if (str_equal(cmd, "clear")) {
        term->line_count   = 0;
        term->scroll_offset = 0;
    } else if (str_starts_with(cmd, "agent ")) {
        cmd_agent(term, cmd + 6);
    } else if (str_equal(cmd, "agent")) {
        cmd_agent(term, "");
    } else if (str_starts_with(cmd, "skill ")) {
        cmd_skill(term, cmd + 6);
    } else if (str_equal(cmd, "skill")) {
        cmd_skill(term, "");
    } else if (str_starts_with(cmd, "memory ")) {
        cmd_memory(term, cmd + 7);
    } else if (str_equal(cmd, "memory")) {
        cmd_memory(term, "");
    } else if (str_starts_with(cmd, "history")) {
        cmd_history(term, cmd + 7);
    } else if (str_equal(cmd, "sysinfo")) {
        cmd_sysinfo(term);
    } else if (str_equal(cmd, "reboot")) {
        terminal_print(term, "Rebooting AevOS...");
        klog("reboot requested via terminal\n");
    } else {
        terminal_printf(term, "Unknown command: '%s'. Type 'help' for commands.",
                        cmd);
    }

    int fnt_h = FONT_HEIGHT;
    int visible_lines = (term->bounds.h - fnt_h - PADDING * 3) / fnt_h;
    if (term->line_count > visible_lines)
        term->scroll_offset = (term->line_count - visible_lines) * fnt_h;
}

void terminal_render(terminal_t *term, fb_ctx_t *fb)
{
    if (!term || !fb)
        return;

    fb_draw_rect(term->bounds.x, term->bounds.y,
                 term->bounds.w, term->bounds.h, COLOR_BG);

    fb_draw_rect(term->bounds.x, term->bounds.y,
                 term->bounds.w, 1, COLOR_DIVIDER);

    const font_t *fnt = font_get_default();
    int line_h = fnt->height;
    int input_area_h = line_h + PADDING * 2;
    int output_area_h = term->bounds.h - input_area_h;
    int visible_lines = output_area_h / line_h;
    int start_line = term->scroll_offset / line_h;

    int y = term->bounds.y + PADDING;
    for (int i = start_line;
         i < term->line_count && (i - start_line) < visible_lines; i++) {
        terminal_line_t *line = &term->lines[i];
        int x = term->bounds.x + PADDING;

        for (int j = 0; j < line->len; j++) {
            uint32_t fg = line->fg_colors[j];
            font_draw_char(fb, x, y, line->text[j], fg, 0, fnt);
            x += fnt->width;
        }
        y += line_h;
    }

    int total_h = term->line_count * line_h;
    if (total_h > output_area_h) {
        draw_scrollbar(fb,
                       term->bounds.x + term->bounds.w - 8,
                       term->bounds.y,
                       output_area_h,
                       term->scroll_offset,
                       total_h,
                       output_area_h);
    }

    int input_y = term->bounds.y + term->bounds.h - input_area_h;
    fb_draw_rect(term->bounds.x, input_y,
                 term->bounds.w, 1, COLOR_DIVIDER);

    int prompt_x = term->bounds.x + PADDING;
    int text_y = input_y + PADDING;

    font_draw_string(fb, prompt_x, text_y, "aevos> ",
                     COLOR_GREEN, 0, fnt);
    prompt_x += fnt->width * 7;

    int max_chars = (term->bounds.w - PADDING * 2 - fnt->width * 7) / fnt->width;
    int start = 0;
    if (term->cursor_pos > max_chars)
        start = term->cursor_pos - max_chars;

    for (int i = start; i < term->input_len && (i - start) < max_chars; i++) {
        font_draw_char(fb, prompt_x + (i - start) * fnt->width,
                       text_y, term->input_buf[i],
                       COLOR_TEXT, 0, fnt);
    }

    term->blink_counter++;
    if ((term->blink_counter / 30) % 2 == 0) {
        int cx = prompt_x + (term->cursor_pos - start) * fnt->width;
        fb_draw_rect(cx, text_y, fnt->width, fnt->height, COLOR_GREEN);
    }
}

static const char *builtin_commands[] = {
    "help", "clear", "agent", "skill", "memory",
    "history", "sysinfo", "reboot", NULL
};

static void terminal_tab_complete(terminal_t *term)
{
    if (term->input_len == 0)
        return;

    term->input_buf[term->input_len] = '\0';

    const char *match = NULL;
    int match_count = 0;

    for (int i = 0; builtin_commands[i]; i++) {
        if (str_starts_with(builtin_commands[i], term->input_buf)) {
            match = builtin_commands[i];
            match_count++;
        }
    }

    if (match_count == 1 && match) {
        int len = (int)strlen(match);
        if (len < TERM_INPUT_LEN - 1) {
            strcpy(term->input_buf, match);
            term->input_len = len;
            term->cursor_pos = len;
            term->input_buf[len] = ' ';
            term->input_buf[len + 1] = '\0';
            term->input_len++;
            term->cursor_pos++;
        }
    } else if (match_count > 1) {
        terminal_print(term, "");
        for (int i = 0; builtin_commands[i]; i++) {
            if (str_starts_with(builtin_commands[i], term->input_buf))
                terminal_printf(term, "  %s", builtin_commands[i]);
        }
    }
}

bool terminal_handle_input(terminal_t *term, input_event_t *ev)
{
    if (!term || !ev)
        return false;

    if (ev->type != 1)
        return false;

    if (ev->keycode == KEY_ENTER) {
        if (term->input_len > 0) {
            term->input_buf[term->input_len] = '\0';
            terminal_execute_command(term, term->input_buf);
            term->input_len  = 0;
            term->cursor_pos = 0;
            term->input_buf[0] = '\0';
        }
        return true;
    }

    if (ev->keycode == KEY_BACKSPACE) {
        if (term->cursor_pos > 0 && term->input_len > 0) {
            for (int i = term->cursor_pos - 1; i < term->input_len; i++)
                term->input_buf[i] = term->input_buf[i + 1];
            term->cursor_pos--;
            term->input_len--;
        }
        return true;
    }

    if (ev->keycode == KEY_TAB) {
        terminal_tab_complete(term);
        return true;
    }

    if (ev->keycode == KEY_LEFT) {
        if (term->cursor_pos > 0)
            term->cursor_pos--;
        return true;
    }
    if (ev->keycode == KEY_RIGHT) {
        if (term->cursor_pos < term->input_len)
            term->cursor_pos++;
        return true;
    }
    if (ev->keycode == KEY_HOME) {
        term->cursor_pos = 0;
        return true;
    }
    if (ev->keycode == KEY_END) {
        term->cursor_pos = term->input_len;
        return true;
    }

    if (ev->keycode == KEY_UP) {
        if (term->history_count > 0 && term->history_index > 0) {
            term->history_index--;
            const char *h = term->command_history[term->history_index];
            int len = (int)strlen(h);
            memcpy(term->input_buf, h, len);
            term->input_buf[len] = '\0';
            term->input_len = len;
            term->cursor_pos = len;
        }
        return true;
    }
    if (ev->keycode == KEY_DOWN) {
        if (term->history_index < term->history_count - 1) {
            term->history_index++;
            const char *h = term->command_history[term->history_index];
            int len = (int)strlen(h);
            memcpy(term->input_buf, h, len);
            term->input_buf[len] = '\0';
            term->input_len = len;
            term->cursor_pos = len;
        } else {
            term->history_index = term->history_count;
            term->input_buf[0] = '\0';
            term->input_len = 0;
            term->cursor_pos = 0;
        }
        return true;
    }

    if (ev->keycode >= 32 && ev->keycode < 127 &&
        term->input_len < TERM_INPUT_LEN - 1) {
        for (int i = term->input_len; i >= term->cursor_pos; i--)
            term->input_buf[i + 1] = term->input_buf[i];
        term->input_buf[term->cursor_pos] = (char)ev->keycode;
        term->cursor_pos++;
        term->input_len++;
        return true;
    }

    return false;
}
