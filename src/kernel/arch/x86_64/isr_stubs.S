/*
 * ISR / IRQ stubs for x86_64 — GAS syntax, long mode
 *
 * IRQ stubs push a dummy error code and the vector number,
 * save all GPRs, call the C dispatcher isr_dispatch(), restore, iretq.
 */

.text
.code64

.extern isr_dispatch

/*
 * IRQ stubs for the PIC-remapped vectors:
 *   IRQ0  = vector 0x20 (timer)
 *   IRQ1  = vector 0x21 (keyboard)
 *   IRQ12 = vector 0x2C (mouse)
 */

.macro IRQ_STUB name, vector
    .global \name
    \name:
        pushq $0              /* dummy error code */
        pushq $\vector        /* interrupt vector  */
        jmp   isr_common
.endm

IRQ_STUB irq0_stub,  0x20
IRQ_STUB irq1_stub,  0x21
IRQ_STUB irq12_stub, 0x2C

isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq  %rsp, %rdi           /* arg0 = pointer to saved regs + vector */

    /* Read the vector number from the stack frame */
    movq  120(%rsp), %rsi      /* arg1 = vector (15 regs * 8 = 120 bytes above) */

    movq  %rsp, %rbp
    andq  $~0xF, %rsp          /* 16-byte align for System V ABI */
    call  isr_dispatch
    movq  %rbp, %rsp

    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %r11
    popq  %r10
    popq  %r9
    popq  %r8
    popq  %rbp
    popq  %rdi
    popq  %rsi
    popq  %rdx
    popq  %rcx
    popq  %rbx
    popq  %rax

    addq  $16, %rsp            /* pop vector + error code */
    iretq

.section .note.GNU-stack, "", @progbits
