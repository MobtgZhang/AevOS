#pragma once

#include <aevos/types.h>

/*
 * Architecture abstraction layer.
 * Provides common primitives that each arch implements.
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

static inline void arch_halt(void)      { __asm__ volatile("wfi"); }
static inline void arch_idle(void)      { __asm__ volatile("wfi"); }
static inline void arch_spin_hint(void) { __asm__ volatile("yield"); }
static inline void arch_panic_stop(void){ __asm__ volatile("msr daifset, #15\n\twfi"); }

#elif defined(__riscv)

static inline void arch_halt(void)      { __asm__ volatile("wfi"); }
static inline void arch_idle(void)      { __asm__ volatile("wfi"); }
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
