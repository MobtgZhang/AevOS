/*
 * AevOS — MIPS64 EL direct-boot entry point (kseg0)
 *
 * When QEMU loads the kernel via -kernel, the CPU jumps here.
 * The kseg0 entry address (0xFFFFFFFF80200000) is a sign-extension
 * of 0x80200000 — compatible with QEMU Malta's 32-bit boot stub.
 *
 * We clear BSS, set up a stack, build a synthetic boot_info_t via
 * the C boot shim, and jump to kernel_main.
 */

    .set    noreorder
    .set    mips64r2

    .section .text.boot, "ax"
    .global _start
    .type   _start, @function
    .ent    _start

_start:
    /*
     * Ensure 64-bit kernel segments (KX/SX/UX) and normal exception
     * vectors.  ERL=1 from QEMU reset already grants full access, but
     * clear ERL/BEV and set KX/SX/UX for normal operation.
     */
    mfc0    $t0, $12            /* CP0.Status */
    li      $t1, ~((1 << 22) | (1 << 2))   /* clear BEV, ERL */
    and     $t0, $t0, $t1
    ori     $t0, $t0, 0x00E0   /* set KX(7), SX(6), UX(5) */
    mtc0    $t0, $12
    ehb

    /* ── Clear BSS ── */
    dla     $t0, _bss_start
    dla     $t1, _bss_end
.Lclear_bss:
    beq     $t0, $t1, .Lbss_done
    nop
    sd      $zero, 0($t0)
    daddiu  $t0, $t0, 8
    b       .Lclear_bss
    nop
.Lbss_done:

    /*
     * Stack: kseg0 address for physical 0x0FF80000 (~255.5 MB).
     * lui sign-extends: lui $sp,0x8FF8 → sp = 0xFFFFFFFF_8FF80000.
     */
    lui     $sp, 0x8FF8
    daddiu  $sp, $sp, -16       /* 16-byte aligned */

    /* ── Call boot shim — returns boot_info_t* in $v0 ── */
    jal     mips64_boot_shim
    nop

    /* ── Jump to kernel_main(boot_info_t *bi) ── */
    move    $a0, $v0
    jal     kernel_main
    nop

    /* ── Should never return ── */
.Lhang:
    wait
    b       .Lhang
    nop

    .end    _start
    .size   _start, . - _start
