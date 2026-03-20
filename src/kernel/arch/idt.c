#include "idt.h"
#include "io.h"

static idt_entry_t idt[IDT_ENTRIES];
static idt_ptr_t   idt_ptr;

static irq_handler_t handlers[IDT_ENTRIES];

/* ── Exception names for debug output ─────────────────── */

static const char *exception_names[32] = {
    "Division Error",            "Debug",
    "NMI",                       "Breakpoint",
    "Overflow",                  "Bound Range Exceeded",
    "Invalid Opcode",            "Device Not Available",
    "Double Fault",              "Coprocessor Segment Overrun",
    "Invalid TSS",               "Segment Not Present",
    "Stack-Segment Fault",       "General Protection Fault",
    "Page Fault",                "Reserved",
    "x87 FP Exception",         "Alignment Check",
    "Machine Check",             "SIMD FP Exception",
    "Virtualization Exception",  "Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
    "Hypervisor Injection",      "VMM Communication",
    "Security Exception",        "Reserved",
};

/* ── IDT gate setup ───────────────────────────────────── */

void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector,
                  uint8_t flags) {
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = selector;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = flags;
    idt[vector].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].reserved    = 0;
}

void idt_register_handler(uint8_t vector, irq_handler_t handler) {
    handlers[vector] = handler;
}

/* ── PIC (8259A) ──────────────────────────────────────── */

void pic_init(void) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: start init sequence, expect ICW4 */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, 0x20); io_wait();  /* IRQ 0-7  -> vectors 32-39 */
    outb(PIC2_DATA, 0x28); io_wait();  /* IRQ 8-15 -> vectors 40-47 */

    /* ICW3: master/slave wiring */
    outb(PIC1_DATA, 0x04); io_wait();  /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* cascade identity */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Restore saved masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    outb(port, inb(port) | (1 << irq));
}

void pic_unmask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    outb(port, inb(port) & ~(1 << irq));
}

/* ── C-level interrupt dispatcher ─────────────────────── */

void isr_handler(interrupt_frame_t *frame) {
    uint64_t vec = frame->int_no;

    if (handlers[vec]) {
        handlers[vec](frame);
        if (vec >= IRQ_BASE && vec < IRQ_BASE + 16)
            pic_eoi((uint8_t)(vec - IRQ_BASE));
        return;
    }

    /* Unhandled exception: halt */
    if (vec < 32) {
        /*
         * In a full build this would call kpanic(); for now we just
         * disable interrupts and halt so the fault is observable via
         * a debugger.  The exception name, error code and RIP are
         * left in registers / on the stack.
         */
        (void)exception_names[vec];
        cli();
        for (;;) halt();
    }

    /* Unhandled IRQ: send EOI and return */
    if (vec >= IRQ_BASE && vec < IRQ_BASE + 16)
        pic_eoi((uint8_t)(vec - IRQ_BASE));
}

/* ── IDT initialization ───────────────────────────────── */

void idt_init(void) {
    /* Clear handler table */
    for (int i = 0; i < IDT_ENTRIES; i++)
        handlers[i] = NULL;

    /* Initialize PIC */
    pic_init();

    /* Mask all IRQs, enable selectively later */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    /* Install the first 48 ISR stubs (0-31 exceptions + 0-15 IRQs) */
    for (int i = 0; i < 48; i++)
        idt_set_gate((uint8_t)i, isr_stub_table[i], 0x08, IDT_GATE_INT);

    /* Load IDT */
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;
    idt_flush((uint64_t)&idt_ptr);

    /* Unmask cascade (IRQ2) so slave PIC works */
    pic_unmask(2);
}
