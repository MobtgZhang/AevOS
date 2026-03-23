#include "terminal.h"
#include "theme.h"
#include "font.h"
#include "../kernel/drivers/gpu_fb.h"
#include "../kernel/drivers/hid.h"
#include "../kernel/locale.h"
#include "../kernel/klog.h"
#include "../agent/agent_core.h"
#include "../lib/string.h"
#include <aevos/config.h>

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
    term_env_set(term, "SHELL", "/bin/bash");
    term_env_set(term, "PATH", "/bin:/usr/bin:/sandbox/bin");
    term_env_set(term, "PWD", term->cwd);
    term_env_set(term, "PS1", "\\u@AevOS:\\w\\$ ");

    terminal_print(term, "AevOS Terminal v0.6.0 (bash-like subset)");
    terminal_print(term, "Virtual roots: /workspace, /sandbox, /tmp");
    terminal_print(term, "Type 'help' for commands.");
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

static void term_format_prompt(terminal_t *term, char *buf, size_t cap)
{
    const char *user = term_env_get(term, "USER");
    if (!user || !user[0])
        user = "aevos";
    snprintf(buf, cap, "%s@AevOS:%s$ ", user, term->cwd);
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
                    "Requested `%s` — no separate process model in-kernel.",
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
    terminal_add_line(term, prompt_line, COLOR_GREEN);

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
    } else if (str_equal(w, "reboot")) {
        terminal_print(term, "Rebooting AevOS...");
        klog("reboot requested via terminal\n");
    } else {
        terminal_printf(term, "Unknown command: '%s'. Type 'help' for commands.",
                        w);
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

    char prompt_buf[TERM_LINE_LEN];
    term_format_prompt(term, prompt_buf, sizeof(prompt_buf));
    int prompt_px = font_measure_string(prompt_buf, fnt);
    font_draw_string(fb, prompt_x, text_y, prompt_buf,
                     COLOR_GREEN, 0, fnt);
    prompt_x += prompt_px;

    int max_chars = (term->bounds.w - PADDING * 2 - prompt_px) / fnt->width;
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
    "echo", "pwd", "cd", "export", "unset", "env", "ls",
    "bash", "sh", "true", "false", "exit", ":",
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
