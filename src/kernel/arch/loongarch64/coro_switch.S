/*
 * LoongArch 64-bit coroutine context switch
 *
 * void coro_switch(uint64_t *old_sp  [$a0],
 *                  uint64_t  new_sp  [$a1])
 * void coro_entry_trampoline(void)
 *
 * Callee-saved: $ra, $fp, $s0-$s8 (11 regs = 88 bytes)
 */

    .text
    .global coro_switch
    .global coro_entry_trampoline
    .extern coro_exit

coro_switch:
    addi.d  $sp, $sp, -88
    st.d    $ra,  $sp, 0
    st.d    $fp,  $sp, 8
    st.d    $s0,  $sp, 16
    st.d    $s1,  $sp, 24
    st.d    $s2,  $sp, 32
    st.d    $s3,  $sp, 40
    st.d    $s4,  $sp, 48
    st.d    $s5,  $sp, 56
    st.d    $s6,  $sp, 64
    st.d    $s7,  $sp, 72
    st.d    $s8,  $sp, 80

    st.d    $sp, $a0, 0
    move    $sp, $a1

    ld.d    $ra,  $sp, 0
    ld.d    $fp,  $sp, 8
    ld.d    $s0,  $sp, 16
    ld.d    $s1,  $sp, 24
    ld.d    $s2,  $sp, 32
    ld.d    $s3,  $sp, 40
    ld.d    $s4,  $sp, 48
    ld.d    $s5,  $sp, 56
    ld.d    $s6,  $sp, 64
    ld.d    $s7,  $sp, 72
    ld.d    $s8,  $sp, 80
    addi.d  $sp, $sp, 88
    jr      $ra

coro_entry_trampoline:
    ld.d    $t0, $sp, 0
    ld.d    $a0, $sp, 8
    addi.d  $sp, $sp, 16
    jirl    $ra, $t0, 0
    bl      coro_exit
    idle    0
