/*
 * AArch64 data cache maintenance for guest RAM visible to virtio (DMA).
 */
#include "../arch.h"

#if defined(__aarch64__)

#define CACHE_LINE 64u

void arch_dcache_flush_range(const void *addr, size_t len)
{
    if (!addr || len == 0)
        return;
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end   = (uintptr_t)addr + len;
    for (uintptr_t p = start; p < end; p += CACHE_LINE)
        __asm__ __volatile__("dc cvac, %0" :: "r"(p) : "memory");
    __asm__ __volatile__("dsb sy" ::: "memory");
}

void arch_dcache_invalidate_range(void *addr, size_t len)
{
    if (!addr || len == 0)
        return;
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(CACHE_LINE - 1);
    uintptr_t end   = (uintptr_t)addr + len;
    for (uintptr_t p = start; p < end; p += CACHE_LINE)
        __asm__ __volatile__("dc ivac, %0" :: "r"(p) : "memory");
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

#endif /* __aarch64__ */
