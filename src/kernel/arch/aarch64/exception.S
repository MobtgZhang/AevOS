/*
 * AArch64 Exception Vector Table
 *
 * Each vector entry is 128 bytes (32 instructions).
 * The table must be 2048-byte aligned.
 *
 * Layout (4 groups × 4 vectors):
 *   Group 0: Current EL with SP_EL0  (Sync, IRQ, FIQ, SError)
 *   Group 1: Current EL with SPx     (Sync, IRQ, FIQ, SError)
 *   Group 2: Lower EL AArch64        (Sync, IRQ, FIQ, SError)
 *   Group 3: Lower EL AArch32        (Sync, IRQ, FIQ, SError)
 */

    .text
    .global aarch64_vectors
    .extern aarch64_unhandled_exception
    .extern timer_handler

/*
 * Macro: generate one vector entry (128 bytes = 32 instructions).
 * Saves minimal context, passes the exception type index to C handler.
 */
.macro VECTOR_ENTRY type_num
    .balign 128
    stp     x0,  x1,  [sp, #-16]!
    stp     x2,  x3,  [sp, #-16]!
    stp     x29, x30, [sp, #-16]!
    mov     x0, #\type_num
    bl      aarch64_unhandled_exception
    ldp     x29, x30, [sp], #16
    ldp     x2,  x3,  [sp], #16
    ldp     x0,  x1,  [sp], #16
    eret
    /* pad to 128 bytes (9 insns used, need 23 more NOPs) */
    .rept 23
        nop
    .endr
.endm

/*
 * IRQ vector: calls timer_handler then returns.
 */
.macro IRQ_VECTOR type_num
    .balign 128
    stp     x0,  x1,  [sp, #-16]!
    stp     x2,  x3,  [sp, #-16]!
    stp     x29, x30, [sp, #-16]!
    bl      timer_handler
    ldp     x29, x30, [sp], #16
    ldp     x2,  x3,  [sp], #16
    ldp     x0,  x1,  [sp], #16
    eret
    .rept 24
        nop
    .endr
.endm

    .balign 2048
aarch64_vectors:
    /* ── Current EL with SP_EL0 ── */
    VECTOR_ENTRY 0          /* Synchronous */
    IRQ_VECTOR   1          /* IRQ */
    VECTOR_ENTRY 2          /* FIQ */
    VECTOR_ENTRY 3          /* SError */

    /* ── Current EL with SPx ── */
    VECTOR_ENTRY 4          /* Synchronous */
    IRQ_VECTOR   5          /* IRQ */
    VECTOR_ENTRY 6          /* FIQ */
    VECTOR_ENTRY 7          /* SError */

    /* ── Lower EL AArch64 ── */
    VECTOR_ENTRY 8          /* Synchronous */
    IRQ_VECTOR   9          /* IRQ */
    VECTOR_ENTRY 10         /* FIQ */
    VECTOR_ENTRY 11         /* SError */

    /* ── Lower EL AArch32 ── */
    VECTOR_ENTRY 12         /* Synchronous */
    IRQ_VECTOR   13         /* IRQ */
    VECTOR_ENTRY 14         /* FIQ */
    VECTOR_ENTRY 15         /* SError */
