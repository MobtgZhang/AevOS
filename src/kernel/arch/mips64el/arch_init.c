/*
 * MIPS64 EL CPU initialization.
 */
#include <aevos/types.h>
#include "../arch.h"

void serial_putchar(char c);

static void exc_puts(const char *s) {
    while (*s)
        serial_putchar(*s++);
}

static void exc_puthex(uint64_t val) {
    const char hex[] = "0123456789abcdef";
    exc_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putchar(hex[(val >> i) & 0xF]);
}

void mips64_unhandled_exception(uint64_t cause, uint64_t epc, uint64_t badvaddr) {
    exc_puts("\n\n!!! MIPS64 Exception\n");
    exc_puts("  EPC      = "); exc_puthex(epc); exc_puts("\n");
    exc_puts("  Cause    = "); exc_puthex(cause); exc_puts("\n");
    exc_puts("  BadVAddr = "); exc_puthex(badvaddr); exc_puts("\n");

    uint32_t exccode = (uint32_t)((cause >> 2) & 0x1F);
    exc_puts("  ExcCode  = "); exc_puthex(exccode);
    exc_puts(" (");
    switch (exccode) {
    case  0: exc_puts("Interrupt");  break;
    case  1: exc_puts("TLB Modified"); break;
    case  2: exc_puts("TLB Load");   break;
    case  3: exc_puts("TLB Store");  break;
    case  4: exc_puts("AddrErr Load"); break;
    case  5: exc_puts("AddrErr Store"); break;
    case  6: exc_puts("Bus Error Ifetch"); break;
    case  7: exc_puts("Bus Error Data"); break;
    case  8: exc_puts("Syscall");    break;
    case  9: exc_puts("Breakpoint"); break;
    case 10: exc_puts("Reserved Inst"); break;
    case 11: exc_puts("CopUnusable"); break;
    case 12: exc_puts("Overflow");   break;
    case 13: exc_puts("Trap");       break;
    default: exc_puts("other");      break;
    }
    exc_puts(")\n  HALT.\n");
    for (;;)
        __asm__ volatile("wait");
}

void arch_early_init(void)  { }

void arch_enable_irq(void)  {
    uint32_t sr;
    __asm__ volatile("mfc0 %0, $12" : "=r"(sr));
    sr |= 0x1;  /* IE bit */
    __asm__ volatile("mtc0 %0, $12" : : "r"(sr));
}

void arch_disable_irq(void) {
    uint32_t sr;
    __asm__ volatile("mfc0 %0, $12" : "=r"(sr));
    sr &= ~0x1u;
    __asm__ volatile("mtc0 %0, $12" : : "r"(sr));
}

void gdt_init(void)
{
    /* MIPS has no GDT. Kernel runs in 64-bit kernel mode (xkphys). */
}

void idt_init(void)
{
    /*
     * MIPS64 exceptions use a fixed vector base (or BEV in CP0.Status).
     * For now, rely on the firmware's exception handlers.
     * A proper implementation would set CP0.EBase and install handlers.
     */
}

void pic_init(void)
{
    /* Malta uses the 8259A PIC cascaded pair, stub for now */
}
