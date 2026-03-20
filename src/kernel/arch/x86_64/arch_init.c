/*
 * x86-64 CPU initialization: GDT, IDT, and 8259 PIC.
 */
#include <aevos/types.h>
#include "../io.h"
#include "../arch.h"

void arch_early_init(void)  { /* GDT+IDT+PIC done individually from main */ }
void arch_enable_irq(void)  { sti(); }
void arch_disable_irq(void) { cli(); }

/* ── GDT ─────────────────────────────────────────────────── */

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} PACKED;

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} PACKED;

static struct gdt_entry gdt[5];
static struct gdt_ptr   gdtr;

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran)
{
    gdt[i].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[i].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[i].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt[i].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[i].granularity  = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access      = access;
}

void gdt_init(void)
{
    gdt_set_entry(0, 0, 0,          0,    0);       /* Null */
    gdt_set_entry(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);   /* Kernel code 64-bit */
    gdt_set_entry(2, 0, 0xFFFFFFFF, 0x92, 0xCF);   /* Kernel data */
    gdt_set_entry(3, 0, 0xFFFFFFFF, 0xFA, 0xAF);   /* User code 64-bit */
    gdt_set_entry(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);   /* User data */

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)&gdt;

    __asm__ volatile(
        "lgdt (%0)\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        :
        : "r"(&gdtr)
        : "rax", "memory"
    );
}

/* ── IDT ─────────────────────────────────────────────────── */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} PACKED;

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} PACKED;

#define IDT_ENTRIES 256

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtr;

extern void timer_handler(void);

static void default_isr(void)
{
    __asm__ volatile("iretq");
}

static void idt_set_entry(int i, uint64_t handler, uint16_t sel,
                           uint8_t type_attr)
{
    idt[i].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[i].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[i].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[i].selector    = sel;
    idt[i].ist         = 0;
    idt[i].type_attr   = type_attr;
    idt[i].reserved    = 0;
}

void idt_init(void)
{
    uint64_t default_handler = (uint64_t)default_isr;

    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_entry(i, default_handler, 0x08, 0x8E);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    __asm__ volatile("lidt (%0)" : : "r"(&idtr) : "memory");
}

/* ── 8259 PIC ────────────────────────────────────────────── */

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

void pic_init(void)
{
    outb(PIC1_CMD,  0x11);  io_wait();
    outb(PIC2_CMD,  0x11);  io_wait();
    outb(PIC1_DATA, 0x20);  io_wait();
    outb(PIC2_DATA, 0x28);  io_wait();
    outb(PIC1_DATA, 0x04);  io_wait();
    outb(PIC2_DATA, 0x02);  io_wait();
    outb(PIC1_DATA, 0x01);  io_wait();
    outb(PIC2_DATA, 0x01);  io_wait();

    /* Mask all except IRQ0 (timer) and IRQ1 (keyboard) */
    outb(PIC1_DATA, 0xFC);
    outb(PIC2_DATA, 0xFF);
}
