#pragma once

#include <aevos/types.h>

/*
 * L1 HAL：各 arch 在 arch_init.c / io.h / *.S 中实现具体硬件原语
 *（中断、MMU CSR、idle、协程切换）。对外仅暴露下列通用入口。
 */

void arch_early_init(void);
void arch_enable_irq(void);
void arch_disable_irq(void);

#if defined(__x86_64__)

static inline void arch_halt(void)      { __asm__ volatile("hlt"); }
static inline void arch_idle(void)      { __asm__ volatile("hlt"); }
static inline void arch_spin_hint(void) { __asm__ volatile("pause"); }
static inline void arch_panic_stop(void){ __asm__ volatile("cli; hlt"); }

#elif defined(__aarch64__)

/*
 * GIC + CNTP PPI is not wired yet (see arch_init pic_init).  WFI would block
 * forever with IRQs never asserted; use YIELD so the scheduler / UI loop keep
 * making progress under QEMU virt until timer delivery is enabled.
 */
static inline void arch_halt(void)      { __asm__ volatile("wfi"); }
static inline void arch_idle(void)      { __asm__ volatile("yield" ::: "memory"); }
static inline void arch_spin_hint(void) { __asm__ volatile("yield"); }
static inline void arch_panic_stop(void){ __asm__ volatile("msr daifset, #15\n\twfi"); }

#elif defined(__riscv)

/*
 * PLIC / S 级定时器尚未接好（见 arch_init pic_init）；WFI 可能永远等不到中断。
 * 使用轻量 nop 让调度器与 UI 主循环在 QEMU virt 上能推进。
 */
static inline void arch_halt(void)      { __asm__ volatile("wfi"); }
static inline void arch_idle(void)      { __asm__ volatile("nop" ::: "memory"); }
static inline void arch_spin_hint(void) { __asm__ volatile("nop"); }
static inline void arch_panic_stop(void){ __asm__ volatile("csrci mstatus, 8\n\twfi"); }

#elif defined(__loongarch64)

static inline void arch_halt(void)      { __asm__ volatile("idle 0"); }
static inline void arch_idle(void)      { __asm__ volatile("idle 0"); }
static inline void arch_spin_hint(void) { __asm__ volatile("nop"); }
static inline void arch_panic_stop(void){ __asm__ volatile("idle 0"); }

#else
#error "Unsupported architecture"
#endif

/* DMA visibility: AArch64 needs explicit cache maintenance for virtio MMIO DMA. */
#if defined(__aarch64__)
void arch_dcache_flush_range(const void *addr, size_t len);
void arch_dcache_invalidate_range(void *addr, size_t len);
#else
static inline void arch_dcache_flush_range(const void *addr, size_t len)
{
    (void)addr;
    (void)len;
}
static inline void arch_dcache_invalidate_range(void *addr, size_t len)
{
    (void)addr;
    (void)len;
}
#endif
