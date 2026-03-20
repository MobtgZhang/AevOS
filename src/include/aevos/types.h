#pragma once

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

typedef uint64_t           size_t;
typedef int64_t            ssize_t;
typedef uint64_t           uintptr_t;
typedef int64_t            intptr_t;
typedef int64_t            ptrdiff_t;

typedef _Bool              bool;
#define true  1
#define false 0

#define NULL ((void *)0)

#define UINT8_MAX  0xFF
#define UINT16_MAX 0xFFFF
#define UINT32_MAX 0xFFFFFFFFU
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
#define INT32_MAX  0x7FFFFFFF
#define INT64_MAX  0x7FFFFFFFFFFFFFFFLL
#define SIZE_MAX   UINT64_MAX

#define offsetof(type, member) __builtin_offsetof(type, member)
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define ALIGN_UP(x, align)   (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align) ((x) & ~((align) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define PACKED   __attribute__((packed))
#define NORETURN __attribute__((noreturn))
#define UNUSED   __attribute__((unused))
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ── Architecture-portable spinlock ──────────────────── */

typedef volatile uint32_t spinlock_t;

static inline void arch_spin_hint_inline(void) {
#if defined(__x86_64__)
    __asm__ volatile("pause");
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__riscv)
    __asm__ volatile("nop");
#elif defined(__loongarch64)
    __asm__ volatile("nop");
#elif defined(__mips64)
    __asm__ volatile("nop");
#endif
}

static inline void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) arch_spin_hint_inline();
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(lock);
}

static inline bool spin_try_lock(spinlock_t *lock) {
    return __sync_lock_test_and_set(lock, 1) == 0;
}

#define SPINLOCK_INIT 0

#define ESKILL_FAIL      1
#define ENOMEM           2
#define EINVAL           3
#define EIO              4
#define ENOENT           5
#define EBUSY            6
#define ENOSPC           7
#define EPERM            8
#define EEXIST           9
#define ENOTSUP         10
