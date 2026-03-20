/*
 * LoongArch 64-bit CPU initialization.
 */
#include <aevos/types.h>
#include "../arch.h"

void arch_early_init(void)  { }
void arch_enable_irq(void)  {
    uint64_t val;
    __asm__ volatile("csrrd %0, 0x4" : "=r"(val));
    val |= 0xFUL;
    __asm__ volatile("csrwr %0, 0x4" : : "r"(val));
}
void arch_disable_irq(void) { __asm__ volatile("csrwr $zero, 0x4"); }

void gdt_init(void)
{
    /* LoongArch has no GDT. Protection via CSR-based page table config. */
}

void idt_init(void)
{
    /* LoongArch uses ECFG/ESTAT CSRs for exception configuration, stub */
}

void pic_init(void)
{
    /* LoongArch uses Extended I/O Interrupt Controller, stub for now */
}
