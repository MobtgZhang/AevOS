/*
 * aarch64 coroutine context switch
 *
 * void coro_switch(uint64_t *old_sp  [x0],
 *                  uint64_t  new_sp  [x1])
 * void coro_entry_trampoline(void)
 *
 * Callee-saved: x19-x28, x29(fp), x30(lr)
 */

    .text
    .global coro_switch
    .global coro_entry_trampoline
    .extern coro_exit

coro_switch:
    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x25, x26, [sp, #-16]!
    stp     x27, x28, [sp, #-16]!
    stp     x29, x30, [sp, #-16]!

    mov     x2, sp
    str     x2, [x0]
    mov     sp, x1

    ldp     x29, x30, [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16
    ret

coro_entry_trampoline:
    ldp     x9, x0, [sp], #16
    blr     x9
    bl      coro_exit
    wfi
