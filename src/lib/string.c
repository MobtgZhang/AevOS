#include "string.h"

/* ── Memory operations ────────────────────────────────────────────── */

void *memset(void *dest, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dest;
    while (n--)
        *d++ = (uint8_t)c;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

/* ── String length ────────────────────────────────────────────────── */

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n])
        n++;
    return n;
}

/* ── String copy ──────────────────────────────────────────────────── */

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (n && (*d++ = *src++))
        n--;
    while (n--)
        *d++ = '\0';
    return dest;
}

/* ── String concatenation ─────────────────────────────────────────── */

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (*d) d++;
    while (n-- && *src)
        *d++ = *src++;
    *d = '\0';
    return dest;
}

/* ── String comparison ────────────────────────────────────────────── */

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* ── String search ────────────────────────────────────────────────── */

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;

    size_t nlen = strlen(needle);
    while (*haystack) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

/* ── Number conversion ────────────────────────────────────────────── */

static int _isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

int atoi(const char *s)
{
    while (_isspace(*s)) s++;
    int sign = 1;
    if (*s == '-')      { sign = -1; s++; }
    else if (*s == '+') { s++; }

    int result = 0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return sign * result;
}

long atol(const char *s)
{
    while (_isspace(*s)) s++;
    long sign = 1;
    if (*s == '-')      { sign = -1; s++; }
    else if (*s == '+') { s++; }

    long result = 0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return sign * result;
}

static void _reverse(char *s, size_t len)
{
    size_t i = 0, j = len - 1;
    while (i < j) {
        char t = s[i];
        s[i]   = s[j];
        s[j]   = t;
        i++;
        j--;
    }
}

char *itoa(int value, char *buf, int base)
{
    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return buf;
    }

    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char *p = buf;
    bool neg = false;

    unsigned int uval;
    if (value < 0 && base == 10) {
        neg  = true;
        uval = (unsigned int)(-(value + 1)) + 1;
    } else {
        uval = (unsigned int)value;
    }

    do {
        *p++ = digits[uval % (unsigned int)base];
        uval /= (unsigned int)base;
    } while (uval);

    if (neg) *p++ = '-';
    *p = '\0';

    _reverse(buf, (size_t)(p - buf));
    return buf;
}

char *ltoa(long value, char *buf, int base)
{
    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return buf;
    }

    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char *p = buf;
    bool neg = false;

    unsigned long uval;
    if (value < 0 && base == 10) {
        neg  = true;
        uval = (unsigned long)(-(value + 1)) + 1;
    } else {
        uval = (unsigned long)value;
    }

    do {
        *p++ = digits[uval % (unsigned long)base];
        uval /= (unsigned long)base;
    } while (uval);

    if (neg) *p++ = '-';
    *p = '\0';

    _reverse(buf, (size_t)(p - buf));
    return buf;
}

/* ── snprintf – basic printf subset (%d %u %x %lx %llx %s %c %p %%) ── */

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

static size_t _emit_char(char *buf, size_t pos, size_t size, char c)
{
    if (pos < size - 1)
        buf[pos] = c;
    return pos + 1;
}

static size_t _emit_str(char *buf, size_t pos, size_t size, const char *s)
{
    while (*s)
        pos = _emit_char(buf, pos, size, *s++);
    return pos;
}

static size_t _emit_uint(char *buf, size_t pos, size_t size,
                          uint64_t val, int base, bool uppercase,
                          int min_width, char pad)
{
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digits = uppercase ? hi : lo;

    char tmp[64];
    int  len = 0;
    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val) {
            tmp[len++] = digits[val % (uint64_t)base];
            val /= (uint64_t)base;
        }
    }

    while (len < min_width)
        tmp[len++] = pad;

    for (int i = len - 1; i >= 0; i--)
        pos = _emit_char(buf, pos, size, tmp[i]);
    return pos;
}

static size_t _emit_int(char *buf, size_t pos, size_t size, int64_t val,
                         int min_width, char pad)
{
    if (val < 0) {
        pos = _emit_char(buf, pos, size, '-');
        val = -val;
        if (min_width > 0) min_width--;
    }
    return _emit_uint(buf, pos, size, (uint64_t)val, 10, false, min_width, pad);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    if (!buf || size == 0) return 0;

    va_list ap;
    va_start(ap, fmt);

    size_t pos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            pos = _emit_char(buf, pos, size, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        char pad = ' ';
        int  width = 0;

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* length modifiers */
        int longness = 0; /* 0=int, 1=long, 2=long long */
        if (*fmt == 'l') {
            longness = 1;
            fmt++;
            if (*fmt == 'l') {
                longness = 2;
                fmt++;
            }
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v;
            if (longness == 2)      v = va_arg(ap, int64_t);
            else if (longness == 1) v = va_arg(ap, long);
            else                    v = va_arg(ap, int);
            pos = _emit_int(buf, pos, size, v, width, pad);
            break;
        }
        case 'u': {
            uint64_t v;
            if (longness == 2)      v = va_arg(ap, uint64_t);
            else if (longness == 1) v = va_arg(ap, unsigned long);
            else                    v = va_arg(ap, unsigned int);
            pos = _emit_uint(buf, pos, size, v, 10, false, width, pad);
            break;
        }
        case 'x': {
            uint64_t v;
            if (longness == 2)      v = va_arg(ap, uint64_t);
            else if (longness == 1) v = va_arg(ap, unsigned long);
            else                    v = va_arg(ap, unsigned int);
            pos = _emit_uint(buf, pos, size, v, 16, false, width, pad);
            break;
        }
        case 'X': {
            uint64_t v;
            if (longness == 2)      v = va_arg(ap, uint64_t);
            else if (longness == 1) v = va_arg(ap, unsigned long);
            else                    v = va_arg(ap, unsigned int);
            pos = _emit_uint(buf, pos, size, v, 16, true, width, pad);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            pos = _emit_str(buf, pos, size, "0x");
            pos = _emit_uint(buf, pos, size, v, 16, false, 0, '0');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            pos = _emit_str(buf, pos, size, s);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            pos = _emit_char(buf, pos, size, c);
            break;
        }
        case '%':
            pos = _emit_char(buf, pos, size, '%');
            break;
        default:
            pos = _emit_char(buf, pos, size, '%');
            pos = _emit_char(buf, pos, size, *fmt);
            break;
        }
        fmt++;
    }

    if (pos < size)
        buf[pos] = '\0';
    else
        buf[size - 1] = '\0';

    va_end(ap);
    return (int)pos;
}

/* ── Character classification ─────────────────────────────────────── */

int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
