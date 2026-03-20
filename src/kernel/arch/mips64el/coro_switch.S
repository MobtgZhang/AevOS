/*
 * MIPS64 EL coroutine context switch
 *
 * void coro_switch(uint64_t *old_sp  [$a0],
 *                  uint64_t  new_sp  [$a1])
 * void coro_entry_trampoline(void)
 *
 * Callee-saved (N64 ABI): $s0-$s7, $gp, $fp, $ra (11 regs = 88 bytes)
 */

    .set    noreorder
    .text
    .global coro_switch
    .global coro_entry_trampoline
    .extern coro_exit

coro_switch:
    daddiu  $sp, $sp, -88
    sd      $ra,  0($sp)
    sd      $fp,  8($sp)
    sd      $s0, 16($sp)
    sd      $s1, 24($sp)
    sd      $s2, 32($sp)
    sd      $s3, 40($sp)
    sd      $s4, 48($sp)
    sd      $s5, 56($sp)
    sd      $s6, 64($sp)
    sd      $s7, 72($sp)
    sd      $gp, 80($sp)

    sd      $sp, 0($a0)
    move    $sp, $a1

    ld      $ra,  0($sp)
    ld      $fp,  8($sp)
    ld      $s0, 16($sp)
    ld      $s1, 24($sp)
    ld      $s2, 32($sp)
    ld      $s3, 40($sp)
    ld      $s4, 48($sp)
    ld      $s5, 56($sp)
    ld      $s6, 64($sp)
    ld      $s7, 72($sp)
    ld      $gp, 80($sp)
    daddiu  $sp, $sp, 88
    jr      $ra
    nop

coro_entry_trampoline:
    ld      $t9, 0($sp)
    ld      $a0, 8($sp)
    daddiu  $sp, $sp, 16
    jalr    $t9
    nop
    j       coro_exit
    nop
    wait
