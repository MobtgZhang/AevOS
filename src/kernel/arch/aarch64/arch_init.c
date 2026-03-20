/*
 * aarch64 CPU initialization.
 * GDT does not exist on ARM64 — these are stubs / equivalent setup.
 */
#include <aevos/types.h>
#include "../arch.h"

void arch_early_init(void)  { }
void arch_enable_irq(void)  { __asm__ volatile("msr daifclr, #15"); }
void arch_disable_irq(void) { __asm__ volatile("msr daifset, #15"); }

void gdt_init(void)
{
    /* ARM64 has no GDT. Memory attributes configured via MAIR_EL1. */
    uint64_t mair = (0x00ULL << 0) |   /* Attr0: Device-nGnRnE */
                    (0x44ULL << 8) |   /* Attr1: Normal Non-Cacheable */
                    (0xFFULL << 16);   /* Attr2: Normal WB Cacheable */
    __asm__ volatile("msr mair_el1, %0" : : "r"(mair));
    __asm__ volatile("isb");
}

void idt_init(void)
{
    /* ARM64 uses a vector table at VBAR_EL1, minimal stub for now */
}

void pic_init(void)
{
    /* ARM64 uses GIC (Generic Interrupt Controller), stub for now */
}
