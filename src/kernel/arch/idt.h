#pragma once

/*
 * Interrupt controller initialization.
 * On x86-64: sets up IDT and 8259 PIC.
 * On aarch64: sets up GIC (Generic Interrupt Controller).
 * On riscv64: sets up PLIC.
 * On loongarch64: sets up interrupt controller.
 */
void idt_init(void);
void pic_init(void);
