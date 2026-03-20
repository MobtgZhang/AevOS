/*
 * x86-64 IRQ stub wrappers.
 *
 * Each stub saves caller-saved registers, calls the C handler,
 * sends EOI to the 8259 PIC, restores registers, and iretq.
 *
 * IRQ0  (vector 0x20) — PIT timer
 * IRQ1  (vector 0x21) — PS/2 keyboard
 * IRQ12 (vector 0x2C) — PS/2 mouse  (slave PIC IRQ4)
 */

.section .text
.code64

/* ── IRQ0: Timer ───────────────────────────────────────── */

.global irq0_stub
irq0_stub:
    push %rax
    push %rcx
    push %rdx
    push %rdi
    push %rsi
    push %r8
    push %r9
    push %r10
    push %r11
    cld

    call timer_handler

    /* EOI to master PIC */
    mov  $0x20, %al
    outb %al,   $0x20

    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rsi
    pop  %rdi
    pop  %rdx
    pop  %rcx
    pop  %rax
    iretq

/* ── IRQ1: Keyboard ────────────────────────────────────── */

.global irq1_stub
irq1_stub:
    push %rax
    push %rcx
    push %rdx
    push %rdi
    push %rsi
    push %r8
    push %r9
    push %r10
    push %r11
    cld

    call keyboard_handler

    /* EOI to master PIC */
    mov  $0x20, %al
    outb %al,   $0x20

    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rsi
    pop  %rdi
    pop  %rdx
    pop  %rcx
    pop  %rax
    iretq

/* ── IRQ12: Mouse ──────────────────────────────────────── */

.global irq12_stub
irq12_stub:
    push %rax
    push %rcx
    push %rdx
    push %rdi
    push %rsi
    push %r8
    push %r9
    push %r10
    push %r11
    cld

    call mouse_handler

    /* EOI to slave PIC, then master PIC */
    mov  $0x20, %al
    outb %al,   $0xA0
    outb %al,   $0x20

    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rsi
    pop  %rdi
    pop  %rdx
    pop  %rcx
    pop  %rax
    iretq

/* Mark stack as non-executable */
.section .note.GNU-stack,"",@progbits
