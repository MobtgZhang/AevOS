/*
 * aarch64 CPU initialization.
 * GDT does not exist on ARM64 — these are stubs / equivalent setup.
 */
#include <aevos/types.h>
#include "../arch.h"

extern void aarch64_vectors(void);

static const char *exc_type_names[] = {
    "SP0 Sync", "SP0 IRQ", "SP0 FIQ", "SP0 SError",
    "SPx Sync", "SPx IRQ", "SPx FIQ", "SPx SError",
    "Lower64 Sync", "Lower64 IRQ", "Lower64 FIQ", "Lower64 SError",
    "Lower32 Sync", "Lower32 IRQ", "Lower32 FIQ", "Lower32 SError",
};

static const char *data_abort_dfsc[] = {
    [0x00] = "Addr size fault L0",
    [0x01] = "Addr size fault L1",
    [0x02] = "Addr size fault L2",
    [0x03] = "Addr size fault L3",
    [0x04] = "Translation fault L0",
    [0x05] = "Translation fault L1",
    [0x06] = "Translation fault L2",
    [0x07] = "Translation fault L3",
    [0x08] = "Access flag fault L0",
    [0x09] = "Access flag fault L1",
    [0x0A] = "Access flag fault L2",
    [0x0B] = "Access flag fault L3",
    [0x0C] = "Permission fault L0",
    [0x0D] = "Permission fault L1",
    [0x0E] = "Permission fault L2",
    [0x0F] = "Permission fault L3",
    [0x10] = "Synchronous external abort",
    [0x11] = "Synchronous Tag Check Fail",
    [0x14] = "Synchronous external abort L0",
    [0x15] = "Synchronous external abort L1",
    [0x16] = "Synchronous external abort L2",
    [0x17] = "Synchronous external abort L3",
    [0x21] = "Alignment fault",
};

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

void aarch64_unhandled_exception(uint64_t type) {
    uint64_t esr, far, elr;
    __asm__ volatile("mrs %0, esr_el1"  : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1"  : "=r"(far));
    __asm__ volatile("mrs %0, elr_el1"  : "=r"(elr));

    exc_puts("\n\n!!! AArch64 Exception: ");
    if (type < 16)
        exc_puts(exc_type_names[type]);
    exc_puts("\n  ELR_EL1 = "); exc_puthex(elr);
    exc_puts("\n  ESR_EL1 = "); exc_puthex(esr);
    exc_puts("\n  FAR_EL1 = "); exc_puthex(far);

    uint32_t ec  = (uint32_t)((esr >> 26) & 0x3F);
    uint32_t iss = (uint32_t)(esr & 0x1FFFFFF);

    exc_puts("\n  EC = "); exc_puthex(ec);
    exc_puts(" (");
    switch (ec) {
    case 0x00: exc_puts("Unknown"); break;
    case 0x01: exc_puts("WF* trapped"); break;
    case 0x0E: exc_puts("Illegal execution"); break;
    case 0x15: exc_puts("SVC AArch64"); break;
    case 0x20: exc_puts("Instr abort lower EL"); break;
    case 0x21: exc_puts("Instr abort same EL"); break;
    case 0x22: exc_puts("PC alignment"); break;
    case 0x24: exc_puts("Data abort lower EL"); break;
    case 0x25: exc_puts("Data abort same EL"); break;
    case 0x26: exc_puts("SP alignment"); break;
    case 0x2F: exc_puts("SError"); break;
    default:   exc_puts("other"); break;
    }
    exc_puts(")\n");

    if (ec == 0x24 || ec == 0x25) {
        uint32_t dfsc = iss & 0x3F;
        uint32_t wnr  = (iss >> 6) & 1;
        exc_puts("  DFSC = "); exc_puthex(dfsc);
        if (dfsc < sizeof(data_abort_dfsc)/sizeof(data_abort_dfsc[0])
            && data_abort_dfsc[dfsc])
            { exc_puts(" ("); exc_puts(data_abort_dfsc[dfsc]); exc_puts(")"); }
        exc_puts(wnr ? " [WRITE]" : " [READ]");
        exc_puts("\n");
    }

    exc_puts("  HALT.\n");
    for (;;)
        __asm__ volatile("wfe");
}

void arch_early_init(void)  { }
void arch_enable_irq(void)  { __asm__ volatile("msr daifclr, #15"); }
void arch_disable_irq(void) { __asm__ volatile("msr daifset, #15"); }

void gdt_init(void)
{
    uint64_t mair = (0x00ULL << 0) |   /* Attr0: Device-nGnRnE */
                    (0x44ULL << 8) |   /* Attr1: Normal Non-Cacheable */
                    (0xFFULL << 16);   /* Attr2: Normal WB Cacheable */
    __asm__ volatile("msr mair_el1, %0" : : "r"(mair));
    __asm__ volatile("isb");
}

void idt_init(void)
{
    uint64_t vbar = (uint64_t)&aarch64_vectors;
    __asm__ volatile("msr vbar_el1, %0" : : "r"(vbar));
    __asm__ volatile("isb");
}

void pic_init(void)
{
    /* ARM64 uses GIC (Generic Interrupt Controller), stub for now */
}
