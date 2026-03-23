/*
 * RISC-V 64-bit CPU initialization.
 */
#include <aevos/types.h>
#include "../arch.h"
#include "../../klog.h"

void arch_early_init(void)  { }
void arch_enable_irq(void)  { __asm__ volatile("csrsi mstatus, 8"); }
void arch_disable_irq(void) { __asm__ volatile("csrci mstatus, 8"); }

void gdt_init(void)
{
    /* RISC-V has no GDT. Protection is via PMP (Physical Memory Protection). */
}

void idt_init(void)
{
    /* RISC-V uses mtvec/stvec for trap vectors, stub for now */
}

void pic_init(void)
{
    klog("[pic] riscv64: PLIC init deferred (virt timer/PLIC MMIO TBD)\n");
}
