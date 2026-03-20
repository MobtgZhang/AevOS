/*
 * RISC-V 64-bit coroutine context switch
 *
 * void coro_switch(uint64_t *old_sp  [a0],
 *                  uint64_t  new_sp  [a1])
 * void coro_entry_trampoline(void)
 *
 * Callee-saved: ra, s0-s11 (13 regs = 104 bytes)
 */

    .text
    .global coro_switch
    .global coro_entry_trampoline
    .extern coro_exit

coro_switch:
    addi    sp, sp, -104
    sd      ra,   0(sp)
    sd      s0,   8(sp)
    sd      s1,  16(sp)
    sd      s2,  24(sp)
    sd      s3,  32(sp)
    sd      s4,  40(sp)
    sd      s5,  48(sp)
    sd      s6,  56(sp)
    sd      s7,  64(sp)
    sd      s8,  72(sp)
    sd      s9,  80(sp)
    sd      s10, 88(sp)
    sd      s11, 96(sp)

    sd      sp, 0(a0)
    mv      sp, a1

    ld      ra,   0(sp)
    ld      s0,   8(sp)
    ld      s1,  16(sp)
    ld      s2,  24(sp)
    ld      s3,  32(sp)
    ld      s4,  40(sp)
    ld      s5,  48(sp)
    ld      s6,  56(sp)
    ld      s7,  64(sp)
    ld      s8,  72(sp)
    ld      s9,  80(sp)
    ld      s10, 88(sp)
    ld      s11, 96(sp)
    addi    sp, sp, 104
    ret

coro_entry_trampoline:
    ld      a5,  0(sp)
    ld      a0,  8(sp)
    addi    sp, sp, 16
    jalr    a5
    call    coro_exit
    wfi
