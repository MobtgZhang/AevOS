/*
 * x86-64 coroutine context switch (GAS syntax)
 *
 * void coro_switch(uint64_t *old_rsp, uint64_t new_rsp)
 * void coro_entry_trampoline(void)
 */

    .section .note.GNU-stack, "", @progbits
    .text
    .global coro_switch
    .global coro_entry_trampoline
    .extern coro_exit

coro_switch:
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    %rsp, (%rdi)
    movq    %rsi, %rsp

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret

coro_entry_trampoline:
    popq    %rax
    popq    %rdi
    callq   *%rax
    callq   coro_exit
    hlt
