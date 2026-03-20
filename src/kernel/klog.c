#include "klog.h"
#include "drivers/serial.h"
#include "drivers/gpu_fb.h"
#include <aevos/config.h>

/*
 * Serial / FB: use short subsystem tags: [boot], [mm], [ui], … (see main.c).
 */

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

/* ── console state for framebuffer text output ── */

static uint32_t console_col = 0;
static uint32_t console_row = 0;
static uint32_t console_fg  = 0x00CCCCCC;
static uint32_t console_bg  = 0x00000000;

#define CHAR_W 8
#define CHAR_H 16

/* ── tiny number → string helpers ── */

static void klog_putchar(char c);
static void klog_puts(const char *s);
static void klog_put_dec(int64_t val, bool is_signed);
static void klog_put_hex(uint64_t val, int min_digits);
static void klog_put_ptr(uint64_t val);

static void klog_putchar(char c)
{
    serial_putchar(c);

    fb_ctx_t *fb = fb_get_ctx();
    if (!fb || !fb->pixels)
        return;

    uint32_t max_cols = fb->width  / CHAR_W;
    uint32_t max_rows = fb->height / CHAR_H;
    if (max_cols == 0 || max_rows == 0)
        return;

    if (c == '\n') {
        console_col = 0;
        console_row++;
    } else if (c == '\r') {
        console_col = 0;
    } else if (c == '\t') {
        console_col = (console_col + 4) & ~3u;
    } else {
        fb_draw_char(console_col * CHAR_W, console_row * CHAR_H,
                     c, console_fg, console_bg);
        console_col++;
    }

    if (console_col >= max_cols) {
        console_col = 0;
        console_row++;
    }

    if (console_row >= max_rows) {
        fb_scroll(0, fb->height, CHAR_H);
        console_row = max_rows - 1;
    }
}

static void klog_puts(const char *s)
{
    while (*s)
        klog_putchar(*s++);
}

static char digit_char(uint8_t d)
{
    return (d < 10) ? ('0' + d) : ('a' + d - 10);
}

static void klog_put_dec(int64_t val, bool is_signed)
{
    char buf[21];
    int  pos = 0;
    bool neg = false;

    if (is_signed && val < 0) {
        neg = true;
        val = -val;
    }

    uint64_t u = (uint64_t)val;
    if (u == 0) {
        klog_putchar('0');
        return;
    }

    while (u > 0) {
        buf[pos++] = '0' + (u % 10);
        u /= 10;
    }

    if (neg) klog_putchar('-');
    while (pos > 0) klog_putchar(buf[--pos]);
}

static void klog_put_hex(uint64_t val, int min_digits)
{
    char buf[16];
    int  pos = 0;

    if (val == 0) {
        for (int i = 0; i < min_digits; i++) klog_putchar('0');
        return;
    }

    while (val > 0) {
        buf[pos++] = digit_char(val & 0xF);
        val >>= 4;
    }
    while (pos < min_digits) buf[pos++] = '0';
    while (pos > 0) klog_putchar(buf[--pos]);
}

static void klog_put_ptr(uint64_t val)
{
    klog_puts("0x");
    klog_put_hex(val, 1);
}

/* ── printf-style formatter ── */

static void kvformat(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            klog_putchar(*fmt);
            continue;
        }

        fmt++;
        if (!*fmt) break;

        int  longness = 0;
        while (*fmt == 'l') { longness++; fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v;
            if (longness >= 2) v = va_arg(ap, int64_t);
            else               v = va_arg(ap, int);
            klog_put_dec(v, true);
            break;
        }
        case 'u': {
            uint64_t v;
            if (longness >= 2) v = va_arg(ap, uint64_t);
            else               v = va_arg(ap, unsigned int);
            klog_put_dec((int64_t)v, false);
            break;
        }
        case 'x': {
            uint64_t v;
            if (longness >= 2) v = va_arg(ap, uint64_t);
            else if (longness == 1) v = va_arg(ap, unsigned long);
            else               v = va_arg(ap, unsigned int);
            klog_put_hex(v, 1);
            break;
        }
        case 'p':
            klog_put_ptr((uint64_t)va_arg(ap, void *));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            klog_puts(s ? s : "(null)");
            break;
        }
        case 'c':
            klog_putchar((char)va_arg(ap, int));
            break;
        case '%':
            klog_putchar('%');
            break;
        default:
            klog_putchar('%');
            klog_putchar(*fmt);
            break;
        }
    }
}

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvformat(fmt, ap);
    va_end(ap);
}

void kpanic(const char *fmt, ...)
{
    klog_puts("\n!!! kernel panic: ");
    va_list ap;
    va_start(ap, fmt);
    kvformat(fmt, ap);
    va_end(ap);
    klog_puts("\nSystem halted.\n");
    for (;;) {
#if defined(__x86_64__)
        __asm__ volatile("cli; hlt");
#elif defined(__aarch64__)
        __asm__ volatile("msr daifset, #15\n\twfi");
#elif defined(__riscv)
        __asm__ volatile("csrci mstatus, 8\n\twfi");
#elif defined(__loongarch64)
        __asm__ volatile("idle 0");
#elif defined(__mips64)
        __asm__ volatile("wait");
#else
        while(1);
#endif
    }
}
