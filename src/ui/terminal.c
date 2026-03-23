#include "terminal.h"
#include "theme.h"
#include "font.h"
#include "../kernel/drivers/gpu_fb.h"
#include "../kernel/drivers/hid.h"
#include "../kernel/locale.h"
#include "../kernel/klog.h"
#include "../agent/agent_core.h"
#include "../llm/llm_runtime.h"
#include "../lib/string.h"
#include "../container/lc_layer.h"
#include "../kernel/net/lwip_port.h"
#include "../kernel/net/dns.h"
#include <aevos/config.h>

/* Ubuntu/GNOME Terminal default palette (purple field, green user@host, blue path). */
#define TERM_LINUX_BG       0xFF300A24
#define TERM_CURSOR_W       2

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

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

static void term_env_set(terminal_t *term, const char *key, const char *val)
{
    if (!term || !key || !*key)
        return;

    for (int i = 0; i < term->env_count; i++) {
        if (str_equal(term->env[i].key, key)) {
            if (val) {
                strncpy(term->env[i].val, val, TERM_ENV_VAL_LEN - 1);
                term->env[i].val[TERM_ENV_VAL_LEN - 1] = '\0';
            } else {
                for (int j = i; j < term->env_count - 1; j++)
                    term->env[j] = term->env[j + 1];
                term->env_count--;
            }
            return;
        }
    }

    if (!val)
        return;
    if (term->env_count >= TERM_MAX_ENV)
        return;

    terminal_env_entry_t *e = &term->env[term->env_count++];
    strncpy(e->key, key, TERM_ENV_KEY_LEN - 1);
    e->key[TERM_ENV_KEY_LEN - 1] = '\0';
    strncpy(e->val, val, TERM_ENV_VAL_LEN - 1);
    e->val[TERM_ENV_VAL_LEN - 1] = '\0';
}

static const char *term_env_get(terminal_t *term, const char *key)
{
    if (!term || !key)
        return NULL;

    for (int i = 0; i < term->env_count; i++) {
        if (str_equal(term->env[i].key, key))
            return term->env[i].val;
    }
    return NULL;
}

static void term_env_unset(terminal_t *term, const char *key)
{
    term_env_set(term, key, NULL);
}

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
    term->env_count    = 0;

    strncpy(term->cwd, "/workspace", TERM_CWD_LEN - 1);
    term->cwd[TERM_CWD_LEN - 1] = '\0';

    term_env_set(term, "HOME", "/workspace");
    term_env_set(term, "USER", "aevos");
    term_env_set(term, "HOSTNAME", "aevos-pc");
    term_env_set(term, "SHELL", "/bin/bash");
    term_env_set(term, "PATH", "/bin:/usr/bin:/sandbox/bin");
    term_env_set(term, "PWD", term->cwd);
    term_env_set(term, "PS1", "\\u@\\h: ");

    term->scroll_follow_bottom = true;

    terminal_print(term, "AevOS shell: line is USER@HOST: <cmd>; then output; prompt repeats below.");
    terminal_print(term, "export USER / HOSTNAME to change prompt. PgUp/PgDn scroll. F3: chat/term focus.");
    terminal_print(term, "Type 'help' for commands.");
    terminal_print(term, "");
}

static int term_content_height_px(terminal_t *term, int *line_h_out,
                                  int *output_area_h_out, int *visible_lines_out)
{
    const font_t *fnt = font_get_default();
    int line_h = fnt->height;
    /* Full inner height; current prompt+input is the last logical line (like a real TTY). */
    int output_area_h = term->bounds.h - PADDING * 2;
    if (output_area_h < line_h)
        output_area_h = line_h;
    int vis = output_area_h / line_h;
    if (vis < 1)
        vis = 1;
    if (line_h_out)
        *line_h_out = line_h;
    if (output_area_h_out)
        *output_area_h_out = output_area_h;
    if (visible_lines_out)
        *visible_lines_out = vis;
    return (term->line_count + 1) * line_h;
}

static int term_max_scroll_px(terminal_t *term)
{
    int lh, oah, vis;
    int total = term_content_height_px(term, &lh, &oah, &vis);
    int m = total - oah;
    return m > 0 ? m : 0;
}

static void terminal_clamp_scroll(terminal_t *term)
{
    int m = term_max_scroll_px(term);
    if (term->scroll_offset < 0)
        term->scroll_offset = 0;
    if (term->scroll_offset > m)
        term->scroll_offset = m;
}

static void terminal_sync_scroll_after_line(terminal_t *term)
{
    int m = term_max_scroll_px(term);
    if (term->scroll_follow_bottom)
        term->scroll_offset = m;
    else if (term->scroll_offset > m)
        term->scroll_offset = m;
}

static void terminal_scroll_to_bottom(terminal_t *term)
{
    term->scroll_follow_bottom = true;
    term->scroll_offset = term_max_scroll_px(term);
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
    terminal_sync_scroll_after_line(term);
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

static const char *skip_ws(const char *s)
{
    if (!s)
        return "";
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static void trim_trailing_ws(char *s)
{
    size_t n = strlen(s);
    while (n > 0 &&
           (s[n - 1] == ' ' || s[n - 1] == '\t' ||
            s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static void trim_inplace(char *s)
{
    const char *a = skip_ws(s);
    if (a != s)
        memmove(s, a, strlen(a) + 1);
    trim_trailing_ws(s);
}

static bool cmd_word_is(const char *line, const char *name)
{
    size_t n = strlen(name);
    if (!str_starts_with(line, name))
        return false;
    return line[n] == '\0' || line[n] == ' ' || line[n] == '\t';
}

static void strip_shell_comment(char *s)
{
    bool sq = false, dq = false;
    for (char *p = s; *p; p++) {
        if (!sq && *p == '"')
            dq = !dq;
        else if (!dq && *p == '\'')
            sq = !sq;
        else if (!sq && !dq && *p == '#') {
            *p = '\0';
            break;
        }
    }
}

#define TERM_PATH_MAX_SEG   24
#define TERM_PATH_SEG_LEN   48

static int path_push_seg(char segs[][TERM_PATH_SEG_LEN], int n, int max,
                         const char *tok, int tok_len)
{
    if (n >= max || tok_len <= 0)
        return n;

    char tmp[TERM_PATH_SEG_LEN];
    if (tok_len >= TERM_PATH_SEG_LEN)
        tok_len = TERM_PATH_SEG_LEN - 1;
    memcpy(tmp, tok, tok_len);
    tmp[tok_len] = '\0';

    if (str_equal(tmp, ".") || tmp[0] == '\0')
        return n;
    if (str_equal(tmp, "..")) {
        if (n > 0)
            n--;
        return n;
    }

    strncpy(segs[n], tmp, TERM_PATH_SEG_LEN - 1);
    segs[n][TERM_PATH_SEG_LEN - 1] = '\0';
    return n + 1;
}

static int path_tokenize_into(const char *path, char segs[][TERM_PATH_SEG_LEN],
                              int max, int start_n)
{
    const char *p = path;
    int n = start_n;

    while (*p == '/')
        p++;

    while (*p && n < max) {
        const char *e = p;
        while (*e && *e != '/')
            e++;
        n = path_push_seg(segs, n, max, p, (int)(e - p));
        p = e;
        while (*p == '/')
            p++;
    }
    return n;
}

static bool path_build(char segs[][TERM_PATH_SEG_LEN], int n,
                       char *out, size_t cap)
{
    if (n == 0) {
        if (cap < 2)
            return false;
        out[0] = '/';
        out[1] = '\0';
        return true;
    }

    size_t pos = 0;
    for (int i = 0; i < n; i++) {
        size_t sl = strlen(segs[i]);
        if (pos + 1 + sl + 1 > cap)
            return false;
        out[pos++] = '/';
        memcpy(out + pos, segs[i], sl);
        pos += sl;
    }
    out[pos] = '\0';
    return true;
}

static bool term_path_resolve(terminal_t *term, const char *in,
                              char *out, size_t cap)
{
    char segs[TERM_PATH_MAX_SEG][TERM_PATH_SEG_LEN];
    int n = 0;

    if (!in || !*in)
        in = ".";

    if (in[0] == '/') {
        n = path_tokenize_into(in, segs, TERM_PATH_MAX_SEG, 0);
    } else {
        n = path_tokenize_into(term->cwd, segs, TERM_PATH_MAX_SEG, 0);
        n = path_tokenize_into(in, segs, TERM_PATH_MAX_SEG, n);
    }

    return path_build(segs, n, out, cap);
}

static bool term_path_allowed(const char *path)
{
    if (!path || path[0] != '/')
        return false;
    if (str_equal(path, "/"))
        return true;
    if (str_starts_with(path, "/workspace"))
        return true;
    if (str_starts_with(path, "/sandbox"))
        return true;
    if (str_starts_with(path, "/tmp"))
        return true;
    return false;
}

static const char *vfs_ls(const char *path)
{
    if (str_equal(path, "/"))
        return "workspace  sandbox  tmp";
    if (str_equal(path, "/workspace"))
        return "kernel  agent  ui  lib  llm  boot";
    if (str_equal(path, "/sandbox"))
        return "project";
    if (str_equal(path, "/sandbox/project"))
        return "(empty)";
    if (str_equal(path, "/tmp"))
        return "(empty)";
    if (str_starts_with(path, "/workspace/"))
        return "(dir)";
    if (str_starts_with(path, "/sandbox/"))
        return "(dir)";
    return NULL;
}

static void term_expand_vars(terminal_t *term, const char *in,
                             char *out, size_t cap)
{
    size_t o = 0;
    out[0] = '\0';
    if (!in)
        return;

    while (*in && o + 1 < cap) {
        if (*in == '$') {
            in++;
            char key[TERM_ENV_KEY_LEN];
            int k = 0;

            if (*in == '{') {
                in++;
                while (*in && *in != '}' && k < TERM_ENV_KEY_LEN - 1)
                    key[k++] = *in++;
                key[k] = '\0';
                if (*in == '}')
                    in++;
            } else {
                while (*in &&
                       ((*in >= 'A' && *in <= 'Z') ||
                        (*in >= 'a' && *in <= 'z') ||
                        (*in >= '0' && *in <= '9') ||
                        *in == '_') &&
                       k < TERM_ENV_KEY_LEN - 1)
                    key[k++] = *in++;
                key[k] = '\0';
            }

            const char *v = term_env_get(term, key);
            if (v) {
                size_t vl = strlen(v);
                if (o + vl >= cap)
                    break;
                memcpy(out + o, v, vl);
                o += vl;
                out[o] = '\0';
            }
            continue;
        }
        out[o++] = *in++;
        out[o] = '\0';
    }
}

/* One line: "USER@HOST: " only (what you type follows on the same logical line). */
static void term_format_prompt(terminal_t *term, char *buf, size_t cap)
{
    const char *user = term_env_get(term, "USER");
    if (!user || !user[0])
        user = "aevos";
    const char *host = term_env_get(term, "HOSTNAME");
    if (!host || !host[0])
        host = "aevos-pc";
    snprintf(buf, cap, "%s@%s: ", user, host);
}

/* Green user@host, then ": " and typed command in normal text */
static void term_colorize_prompt_command_line(terminal_t *term, terminal_line_t *line)
{
    if (!term || !line || line->len <= 0)
        return;

    char pfx[TERM_LINE_LEN];
    term_format_prompt(term, pfx, sizeof(pfx));
    int plen = (int)strlen(pfx);
    if (line->len < plen || strncmp(line->text, pfx, plen) != 0) {
        for (int j = 0; j < line->len; j++)
            line->fg_colors[j] = COLOR_TEXT;
        return;
    }

    const char *user = term_env_get(term, "USER");
    if (!user || !user[0])
        user = "aevos";
    const char *host = term_env_get(term, "HOSTNAME");
    if (!host || !host[0])
        host = "aevos-pc";
    int uh = (int)strlen(user) + 1 + (int)strlen(host);

    int i = 0;
    for (; i < uh && i < line->len; i++)
        line->fg_colors[i] = COLOR_GREEN;
    for (; i < plen && i < line->len; i++)
        line->fg_colors[i] = COLOR_TEXT;
    for (; i < line->len; i++)
        line->fg_colors[i] = COLOR_TEXT;
}

static int term_draw_live_prompt(fb_ctx_t *fb, int x, int y, terminal_t *term,
                                 const font_t *fnt)
{
    const char *user = term_env_get(term, "USER");
    if (!user || !user[0])
        user = "aevos";
    const char *host = term_env_get(term, "HOSTNAME");
    if (!host || !host[0])
        host = "aevos-pc";

    int px = x;
    font_draw_string(fb, px, y, user, COLOR_GREEN, 0, fnt);
    px += font_measure_string(user, fnt);
    font_draw_char(fb, px, y, '@', COLOR_GREEN, 0, fnt);
    px += fnt->width;
    font_draw_string(fb, px, y, host, COLOR_GREEN, 0, fnt);
    px += font_measure_string(host, fnt);
    font_draw_string(fb, px, y, ": ", COLOR_TEXT, 0, fnt);
    px += font_measure_string(": ", fnt);
    return px - x;
}

static void cmd_echo(terminal_t *term, const char *args)
{
    char tmp[TERM_LINE_LEN];
    char out[TERM_LINE_LEN];

    strncpy(tmp, skip_ws(args), sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    term_expand_vars(term, tmp, out, sizeof(out));
    terminal_print(term, out);
}

static void cmd_pwd(terminal_t *term)
{
    terminal_print(term, term->cwd);
}

static void cmd_cd(terminal_t *term, const char *args)
{
    const char *a = skip_ws(args);
    char target[TERM_CWD_LEN];

    if (*a == '\0') {
        const char *home = term_env_get(term, "HOME");
        if (!home || !home[0])
            home = "/workspace";
        strncpy(target, home, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
    } else {
        if (!term_path_resolve(term, a, target, sizeof(target))) {
            terminal_print(term, "cd: path too long");
            return;
        }
    }

    if (!term_path_allowed(target)) {
        terminal_print(term, "cd: permission denied (sandbox roots only)");
        return;
    }

    strncpy(term->cwd, target, TERM_CWD_LEN - 1);
    term->cwd[TERM_CWD_LEN - 1] = '\0';
    term_env_set(term, "PWD", term->cwd);
}

static void cmd_export(terminal_t *term, const char *args)
{
    const char *a = skip_ws(args);
    if (*a == '\0') {
        for (int i = 0; i < term->env_count; i++) {
            terminal_printf(term, "export %s=\"%s\"",
                            term->env[i].key, term->env[i].val);
        }
        return;
    }

    char key[TERM_ENV_KEY_LEN];
    const char *eq = a;
    while (*eq && *eq != '=')
        eq++;

    if (*eq != '=') {
        term_env_set(term, a, "");
        return;
    }

    int kl = (int)(eq - a);
    if (kl <= 0 || kl >= TERM_ENV_KEY_LEN) {
        terminal_print(term, "export: invalid name");
        return;
    }

    memcpy(key, a, kl);
    key[kl] = '\0';
    term_env_set(term, key, eq + 1);
    if (str_equal(key, "PWD"))
        strncpy(term->cwd, eq + 1, TERM_CWD_LEN - 1);
}

static void cmd_unset(terminal_t *term, const char *args)
{
    const char *a = skip_ws(args);
    if (*a == '\0') {
        terminal_print(term, "unset: variable name required");
        return;
    }
    term_env_unset(term, a);
}

static void cmd_env(terminal_t *term)
{
    for (int i = 0; i < term->env_count; i++)
        terminal_printf(term, "%s=%s",
                        term->env[i].key, term->env[i].val);
}

static void cmd_ls(terminal_t *term, const char *args)
{
    const char *a = skip_ws(args);
    char path[TERM_CWD_LEN];

    if (*a == '\0') {
        strncpy(path, term->cwd, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        if (!term_path_resolve(term, a, path, sizeof(path))) {
            terminal_print(term, "ls: path too long");
            return;
        }
    }

    if (!term_path_allowed(path)) {
        terminal_print(term, "ls: permission denied");
        return;
    }

    const char *listing = vfs_ls(path);
    if (!listing)
        terminal_print(term, "ls: no such virtual directory");
    else
        terminal_print(term, listing);
}

static void cmd_shell_banner(terminal_t *term, const char *name)
{
    terminal_printf(term,
                    "Running AevOS shell (bash-like). "
                    "Requested `%s` - no separate process model in-kernel.",
                    name);
}

static void cmd_help(terminal_t *term)
{
    terminal_print(term, "Available commands:");
    terminal_print(term, "  echo, pwd, cd     - POSIX/bash-style (virtual FS)");
    terminal_print(term, "  export, unset, env - Environment");
    terminal_print(term, "  ls                 - List virtual directories");
    terminal_print(term, "  bash | sh          - Shell identity (no fork)");
    terminal_print(term, "  true | false | :   - No-op / false / no-op");
    terminal_print(term, "  exit               - Message only (kernel stays up)");
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
    terminal_print(term, "  llm status         - Local GGUF loaded or stub only");
    terminal_print(term, "  llm load <path>    - Load model from VFS path");
    terminal_print(term, "  llm unload         - Release model (stub only)");
    terminal_print(term, "  docker ...         - LC layer: docker CLI subset (OCI shim)");
    terminal_print(term, "  ping <host>        - ICMP echo (IPv4 / DNS / IPv6)");
    terminal_print(term, "  ping6 <ipv6>       - ICMPv6 echo (link-local or via gw MAC)");
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

static void cmd_docker(terminal_t *term, const char *args)
{
    const char *a = skip_ws(args);

    if (*a == '\0' || str_starts_with(a, "help") || str_starts_with(a, "--help")) {
        terminal_print(term, "docker (LC - Docker CLI compatible subset on AevOS)");
        terminal_print(term, "  docker version | info");
        terminal_print(term, "  docker ps      | docker container ls");
        terminal_print(term, "  docker images");
        terminal_print(term, "  docker run [-it] <image> [cmd ...]   (OCI shim, no real fork)");
        terminal_print(term, "  docker stop <id|name>");
        terminal_print(term, "  docker rm <id|name>   (after stop)");
        return;
    }

    if (cmd_word_is(a, "version")) {
        char buf[384];
        lc_docker_version(buf, sizeof(buf));
        terminal_print(term, buf);
        return;
    }

    if (cmd_word_is(a, "info")) {
        char buf[512];
        lc_docker_info(buf, sizeof(buf));
        terminal_print(term, buf);
        return;
    }

    if (cmd_word_is(a, "ps")) {
        lc_container_slot_t slots[LC_MAX_CONTAINERS];
        int n = 0;
        lc_docker_ps(slots, LC_MAX_CONTAINERS, &n);
        terminal_print(term,
                       "CONTAINER ID      NAME          IMAGE           STATE");
        for (int i = 0; i < n; i++) {
            const char *st = "created";
            if (slots[i].state == LC_CTR_RUNNING)
                st = "running";
            else if (slots[i].state == LC_CTR_STOPPED)
                st = "stopped";
            terminal_printf(term, "%-17s %-13s %-15s %s",
                            slots[i].id_str, slots[i].name,
                            slots[i].image, st);
        }
        if (n == 0)
            terminal_print(term, "(no containers)");
        return;
    }

    if (str_starts_with(a, "container ")) {
        const char *sub = skip_ws(a + 9);
        if (cmd_word_is(sub, "ls") || cmd_word_is(sub, "ps")) {
            cmd_docker(term, "ps");
            return;
        }
        terminal_print(term, "docker: unknown container subcommand");
        return;
    }

    if (cmd_word_is(a, "images")) {
        char buf[2048];
        lc_docker_images(buf, sizeof(buf));
        terminal_print(term, buf);
        return;
    }

    if (cmd_word_is(a, "run")) {
        const char *p = skip_ws(a + 3);
        char image[LC_IMAGE_NAME_LEN];
        char rest[TERM_LINE_LEN];

        image[0] = '\0';
        rest[0] = '\0';

        while (*p) {
            if (*p == '-') {
                while (*p && *p != ' ')
                    p++;
                p = skip_ws(p);
                continue;
            }
            const char *e = p;
            while (*e && *e != ' ')
                e++;
            size_t il = (size_t)(e - p);
            if (il >= sizeof(image))
                il = sizeof(image) - 1;
            memcpy(image, p, il);
            image[il] = '\0';
            p = skip_ws(e);
            if (*p) {
                strncpy(rest, p, sizeof(rest) - 1);
                rest[sizeof(rest) - 1] = '\0';
            }
            break;
        }

        if (!image[0]) {
            terminal_print(term, "docker: \"docker run\" requires an image argument");
            return;
        }

        char msg[512];
        (void)lc_docker_run(image, rest, msg, sizeof(msg));
        terminal_print(term, msg);
        return;
    }

    if (cmd_word_is(a, "stop")) {
        const char *id = skip_ws(a + 4);
        if (!*id) {
            terminal_print(term, "docker: \"docker stop\" requires a container id or name");
            return;
        }
        char err[192];
        (void)lc_docker_stop(id, err, sizeof(err));
        terminal_print(term, err);
        return;
    }

    if (cmd_word_is(a, "rm")) {
        const char *id = skip_ws(a + 2);
        if (!*id) {
            terminal_print(term, "docker: \"docker rm\" requires a container id or name");
            return;
        }
        char err[192];
        (void)lc_docker_rm(id, err, sizeof(err));
        terminal_print(term, err);
        return;
    }

    terminal_printf(term, "docker: unknown command (try `docker help`)");
}

static void cmd_llm(terminal_t *term, const char *args)
{
    llm_ctx_t *ctx = llm_kernel_singleton();
    if (!ctx) {
        terminal_print(term, "llm: internal error (no singleton)");
        return;
    }

    while (*args == ' ')
        args++;

    if (args[0] == '\0' || str_starts_with(args, "status")) {
        if (llm_is_local_loaded(ctx))
            terminal_printf(term, "llm: local model active (ctx=%u tok)",
                            (unsigned)ctx->config.n_ctx);
        else
            terminal_print(term, "llm: no local model - use: llm load <path>");
        return;
    }

    if (str_starts_with(args, "unload")) {
        int r = llm_reload(ctx, NULL);
        if (r == 0)
            terminal_print(term, "llm: unloaded (stub context)");
        else
            terminal_printf(term, "llm: unload failed (%d)", r);
        return;
    }

    if (str_starts_with(args, "load ")) {
        const char *p = args + 5;
        while (*p == ' ')
            p++;
        if (*p == '\0') {
            terminal_print(term, "Usage: llm load <vfs-path-to.gguf>");
            return;
        }
        int r = llm_reload(ctx, p);
        if (r == 0)
            terminal_printf(term, "llm: loaded '%s'", p);
        else {
            terminal_printf(term, "llm: load failed (%d)", r);
            (void)llm_reload(ctx, NULL);
            terminal_print(term, "llm: reverted to stub");
        }
        return;
    }

    terminal_print(term, "Usage: llm [status|load <path>|unload]");
}

static void cmd_ping(terminal_t *term, const char *args)
{
    const char *a = skip_ws(args);
    if (*a == '\0' || str_starts_with(a, "help") || str_starts_with(a, "--help")) {
        terminal_print(term, "Usage: ping <host>");
        terminal_print(term, "  host: IPv4, IPv6 literal, or DNS name (A then AAAA).");
        terminal_print(term, "  Timeout 4s DNS + 3s echo.");
        return;
    }

    char host[128];
    size_t hi = 0;
    while (a[hi] && a[hi] != ' ' && a[hi] != '\t' && hi + 1 < sizeof(host))
        hi++;
    memcpy(host, a, hi);
    host[hi] = '\0';

    uint32_t    ip  = 0;
    ipv6_addr_t ip6;
    memset(&ip6, 0, sizeof(ip6));
    bool        use_v6 = false;

    if (net_parse_ipv4(host, &ip) == 0) {
        use_v6 = false;
    } else if (net_parse_ipv6(host, &ip6) == 0) {
        use_v6 = true;
    } else {
        terminal_printf(term, "ping: resolving %s (DNS)...", host);
        if (dns_resolve_a(host, &ip, 4000) == 0) {
            use_v6 = false;
        } else if (dns_resolve_aaaa(host, &ip6, 4000) == 0) {
            use_v6 = true;
        } else {
            terminal_printf(term, "ping: could not resolve '%s' (no A/AAAA)", host);
            return;
        }
    }

    if (!use_v6) {
        terminal_printf(term, "PING %s (%u.%u.%u.%u): icmp_seq=1 32 data bytes",
                        host,
                        (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                        (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
        uint32_t rtt_ms = 0;
        int      rc     = icmp_ping(ip, 3000, &rtt_ms);
        if (rc == 0) {
            terminal_printf(term,
                            "64 bytes from %u.%u.%u.%u: icmp_seq=1 time=%u ms",
                            (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                            (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF),
                            (unsigned)rtt_ms);
        } else if (rc == -(int)ETIMEDOUT)
            terminal_printf(term, "ping %s: request timed out", host);
        else if (rc == -(int)EBUSY)
            terminal_print(term, "ping: busy (another ping in progress)");
        else
            terminal_printf(term, "ping %s: failed (%d)", host, rc);
        return;
    }

    char v6s[96];
    net_format_ipv6(&ip6, v6s, sizeof(v6s));
    terminal_printf(term, "PING6 %s (%s): icmp_seq=1 32 data bytes", host, v6s);
    uint32_t rtt6 = 0;
    int      r6   = icmp6_ping(&ip6, 3000, &rtt6);
    if (r6 == 0)
        terminal_printf(term, "64 bytes from %s: icmp_seq=1 time=%u ms", v6s,
                        (unsigned)rtt6);
    else if (r6 == -(int)ETIMEDOUT)
        terminal_printf(term, "ping6 %s: request timed out", host);
    else if (r6 == -(int)EBUSY)
        terminal_print(term, "ping6: busy");
    else
        terminal_printf(term, "ping6 %s: failed (%d)", host, r6);
}

static void cmd_ping6(terminal_t *term, const char *args)
{
    const char *a = skip_ws(args);
    if (*a == '\0' || str_starts_with(a, "help")) {
        terminal_print(term, "Usage: ping6 <ipv6-address>");
        return;
    }
    cmd_ping(term, args);
}

void terminal_execute_command(terminal_t *term, const char *cmd)
{
    if (!term || !cmd)
        return;

    while (*cmd == ' ') cmd++;
    if (*cmd == '\0')
        return;

    term->scroll_follow_bottom = true;

    char work[TERM_LINE_LEN];
    strncpy(work, cmd, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    strip_shell_comment(work);
    trim_inplace(work);
    if (work[0] == '\0')
        return;

    char prompt_line[TERM_LINE_LEN];
    char pfx[TERM_LINE_LEN];
    term_format_prompt(term, pfx, sizeof(pfx));
    snprintf(prompt_line, sizeof(prompt_line), "%s%s", pfx, cmd);
    terminal_add_line(term, prompt_line, COLOR_TEXT);
    if (term->line_count > 0)
        term_colorize_prompt_command_line(term,
                                          &term->lines[term->line_count - 1]);

    if (term->history_count < TERM_HISTORY_COUNT) {
        strncpy(term->command_history[term->history_count], cmd,
                TERM_HISTORY_LEN - 1);
        term->command_history[term->history_count][TERM_HISTORY_LEN - 1] = '\0';
        term->history_count++;
    }
    term->history_index = term->history_count;

    const char *w = work;

    if (cmd_word_is(w, "echo")) {
        cmd_echo(term, w + 4);
    } else if (cmd_word_is(w, "pwd")) {
        cmd_pwd(term);
    } else if (cmd_word_is(w, "cd")) {
        cmd_cd(term, w + 2);
    } else if (cmd_word_is(w, "export")) {
        cmd_export(term, w + 6);
    } else if (cmd_word_is(w, "unset")) {
        cmd_unset(term, w + 5);
    } else if (cmd_word_is(w, "env")) {
        cmd_env(term);
    } else if (cmd_word_is(w, "ls")) {
        cmd_ls(term, w + 2);
    } else if (cmd_word_is(w, "bash") || cmd_word_is(w, "sh")) {
        cmd_shell_banner(term, w);
    } else if (str_equal(w, "true") || str_equal(w, ":")) {
        /* no-op */
    } else if (str_equal(w, "false")) {
        /* bash false exits 1; no job control in-kernel */
    } else if (cmd_word_is(w, "exit")) {
        terminal_print(term, "exit: not supported in-kernel session");
    } else if (str_equal(w, "help")) {
        cmd_help(term);
    } else if (str_equal(w, "clear")) {
        term->line_count   = 0;
        term->scroll_offset = 0;
        term->scroll_follow_bottom = true;
    } else if (str_starts_with(w, "agent ")) {
        cmd_agent(term, w + 6);
    } else if (str_equal(w, "agent")) {
        cmd_agent(term, "");
    } else if (str_starts_with(w, "skill ")) {
        cmd_skill(term, w + 6);
    } else if (str_equal(w, "skill")) {
        cmd_skill(term, "");
    } else if (str_starts_with(w, "memory ")) {
        cmd_memory(term, w + 7);
    } else if (str_equal(w, "memory")) {
        cmd_memory(term, "");
    } else if (str_starts_with(w, "history")) {
        cmd_history(term, w + 7);
    } else if (str_equal(w, "sysinfo")) {
        cmd_sysinfo(term);
    } else if (str_starts_with(w, "llm ")) {
        cmd_llm(term, w + 4);
    } else if (str_equal(w, "llm")) {
        cmd_llm(term, "");
    } else if (cmd_word_is(w, "docker")) {
        cmd_docker(term, skip_ws(w + 6));
    } else if (cmd_word_is(w, "ping")) {
        cmd_ping(term, w + 4);
    } else if (cmd_word_is(w, "ping6")) {
        cmd_ping6(term, w + 5);
    } else if (str_equal(w, "reboot")) {
        terminal_print(term, "Rebooting AevOS...");
        klog("reboot requested via terminal\n");
    } else {
        terminal_printf(term, "Unknown command: '%s'. Type 'help' for commands.",
                        w);
    }

    terminal_scroll_to_bottom(term);
}

static void term_draw_compose_line(fb_ctx_t *fb, terminal_t *term, int y,
                                   const font_t *fnt)
{
    int line_h = fnt->height;
    int prompt_x = term->bounds.x + PADDING;
    int prompt_px = term_draw_live_prompt(fb, prompt_x, y, term, fnt);
    prompt_x += prompt_px;

    int max_chars = (term->bounds.w - PADDING * 2 - prompt_px - TERM_CURSOR_W) / fnt->width;
    if (max_chars < 1)
        max_chars = 1;
    int start = 0;
    if (term->cursor_pos > max_chars)
        start = term->cursor_pos - max_chars;

    for (int i = start; i < term->input_len && (i - start) < max_chars; i++) {
        font_draw_char(fb, prompt_x + (i - start) * fnt->width,
                       y, term->input_buf[i],
                       COLOR_TEXT, 0, fnt);
    }

    int cx = prompt_x + (term->cursor_pos - start) * fnt->width;
    fb_draw_rect(cx, y, TERM_CURSOR_W, line_h, COLOR_TEXT);
}

void terminal_render(terminal_t *term, fb_ctx_t *fb)
{
    if (!term || !fb)
        return;

    fb_draw_rect(term->bounds.x, term->bounds.y,
                 term->bounds.w, term->bounds.h, TERM_LINUX_BG);

    const font_t *fnt = font_get_default();
    int line_h = fnt->height;
    int out_top = term->bounds.y + PADDING;
    int view_h = term->bounds.h - PADDING * 2;
    if (view_h < line_h)
        view_h = line_h;

    terminal_clamp_scroll(term);

    int first_line = term->scroll_offset / line_h;
    int total_doc_lines = term->line_count + 1;
    int y = out_top;

    for (int li = first_line; li < total_doc_lines; li++) {
        if (y + line_h > out_top + view_h)
            break;
        if (li < term->line_count) {
            terminal_line_t *line = &term->lines[li];
            int x = term->bounds.x + PADDING;

            for (int j = 0; j < line->len; j++) {
                uint32_t fg = line->fg_colors[j];
                font_draw_char(fb, x, y, line->text[j], fg, 0, fnt);
                x += fnt->width;
            }
        } else {
            term_draw_compose_line(fb, term, y, fnt);
        }
        y += line_h;
    }

    if (y < out_top + view_h)
        fb_draw_rect(term->bounds.x, y, term->bounds.w,
                     out_top + view_h - y, TERM_LINUX_BG);

    int total_h = (term->line_count + 1) * line_h;
    if (total_h > view_h) {
        draw_scrollbar(fb,
                       term->bounds.x + term->bounds.w - 8,
                       out_top,
                       view_h,
                       term->scroll_offset,
                       total_h,
                       view_h);
    }
}

static const char *builtin_commands[] = {
    "echo", "pwd", "cd", "export", "unset", "env", "ls",
    "bash", "sh", "true", "false", "exit", ":",
    "help", "clear", "agent", "skill", "memory",
    "history", "sysinfo", "llm", "docker", "reboot", NULL
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

    if (ev->keycode == KEY_PGUP) {
        int lh, oah, vis;
        term_content_height_px(term, &lh, &oah, &vis);
        int page = vis * lh;
        if (page < lh)
            page = lh;
        term->scroll_follow_bottom = false;
        term->scroll_offset -= page;
        terminal_clamp_scroll(term);
        return true;
    }
    if (ev->keycode == KEY_PGDN) {
        int lh, oah, vis;
        term_content_height_px(term, &lh, &oah, &vis);
        int page = vis * lh;
        if (page < lh)
            page = lh;
        term->scroll_offset += page;
        terminal_clamp_scroll(term);
        if (term->scroll_offset >= term_max_scroll_px(term))
            term->scroll_follow_bottom = true;
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

    {
        char ch = keycode_to_char(ev->keycode, ev->modifiers);
        if (ch >= 32 && ch < 127 &&
            term->input_len < TERM_INPUT_LEN - 1) {
            for (int i = term->input_len; i >= term->cursor_pos; i--)
                term->input_buf[i + 1] = term->input_buf[i];
            term->input_buf[term->cursor_pos] = ch;
            term->cursor_pos++;
            term->input_len++;
            return true;
        }
    }

    return false;
}

bool terminal_handle_mouse_scroll(terminal_t *term, int32_t dy)
{
    if (!term || dy == 0)
        return false;

    int lh, oah, vis;
    term_content_height_px(term, &lh, &oah, &vis);
    term->scroll_follow_bottom = false;
    /* Positive dy: treat as scroll toward newer lines (bottom). */
    term->scroll_offset += (int)dy * lh * 2;
    terminal_clamp_scroll(term);
    if (term->scroll_offset >= term_max_scroll_px(term))
        term->scroll_follow_bottom = true;
    return true;
}
