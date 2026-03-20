;; ==========================================================================
;; isr.asm — ISR / IRQ stubs and GDT/IDT flush routines for x86_64
;; NASM syntax, long mode
;; ==========================================================================

[BITS 64]
[DEFAULT REL]

extern isr_handler          ; void isr_handler(interrupt_frame_t *frame)

;; --------------------------------------------------------------------------
;; Macros for ISR stubs
;; --------------------------------------------------------------------------

;; Exception that does NOT push an error code — we push a dummy zero.
%macro ISR_NOERRCODE 1
    global isr%1
    isr%1:
        push qword 0       ; dummy error code
        push qword %1      ; interrupt number
        jmp isr_common_stub
%endmacro

;; Exception that pushes an error code — only push interrupt number.
%macro ISR_ERRCODE 1
    global isr%1
    isr%1:
        push qword %1      ; interrupt number (error code already on stack)
        jmp isr_common_stub
%endmacro

;; --------------------------------------------------------------------------
;; Exception stubs (vectors 0 – 31)
;; --------------------------------------------------------------------------

ISR_NOERRCODE 0             ; #DE  Divide Error
ISR_NOERRCODE 1             ; #DB  Debug
ISR_NOERRCODE 2             ;      NMI
ISR_NOERRCODE 3             ; #BP  Breakpoint
ISR_NOERRCODE 4             ; #OF  Overflow
ISR_NOERRCODE 5             ; #BR  Bound Range Exceeded
ISR_NOERRCODE 6             ; #UD  Invalid Opcode
ISR_NOERRCODE 7             ; #NM  Device Not Available
ISR_ERRCODE   8             ; #DF  Double Fault
ISR_NOERRCODE 9             ;      Coprocessor Segment Overrun
ISR_ERRCODE   10            ; #TS  Invalid TSS
ISR_ERRCODE   11            ; #NP  Segment Not Present
ISR_ERRCODE   12            ; #SS  Stack-Segment Fault
ISR_ERRCODE   13            ; #GP  General Protection Fault
ISR_ERRCODE   14            ; #PF  Page Fault
ISR_NOERRCODE 15            ;      Reserved
ISR_NOERRCODE 16            ; #MF  x87 FP Exception
ISR_ERRCODE   17            ; #AC  Alignment Check
ISR_NOERRCODE 18            ; #MC  Machine Check
ISR_NOERRCODE 19            ; #XM  SIMD FP Exception
ISR_NOERRCODE 20            ; #VE  Virtualization Exception
ISR_ERRCODE   21            ; #CP  Control Protection
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28            ; #HV  Hypervisor Injection
ISR_ERRCODE   29            ; #VC  VMM Communication
ISR_ERRCODE   30            ; #SX  Security Exception
ISR_NOERRCODE 31

;; --------------------------------------------------------------------------
;; IRQ stubs (vectors 32 – 47,  mapped via PIC to IRQ 0 – 15)
;; --------------------------------------------------------------------------

%assign i 32
%rep 16
    ISR_NOERRCODE i
%assign i i+1
%endrep

;; --------------------------------------------------------------------------
;; Common interrupt stub — saves context, calls C handler, restores, iretq
;; --------------------------------------------------------------------------

isr_common_stub:
    ;; Save all general-purpose registers (matches interrupt_frame_t layout)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ;; First argument (System V ABI) = pointer to interrupt_frame_t
    mov  rdi, rsp

    ;; Align stack to 16 bytes (required by ABI) before call
    mov  rbp, rsp
    and  rsp, ~0xF
    call isr_handler
    mov  rsp, rbp

    ;; Restore general-purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ;; Remove interrupt number and error code from the stack
    add rsp, 16

    iretq

;; --------------------------------------------------------------------------
;; gdt_flush — load a new GDT and reload segment registers
;;   rdi = address of gdt_ptr_t
;; --------------------------------------------------------------------------

global gdt_flush
gdt_flush:
    lgdt [rdi]

    ;; Reload CS through a far return
    push qword 0x08            ; kernel code selector
    lea  rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    mov ax, 0x10               ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

;; --------------------------------------------------------------------------
;; idt_flush — load a new IDT
;;   rdi = address of idt_ptr_t
;; --------------------------------------------------------------------------

global idt_flush
idt_flush:
    lidt [rdi]
    ret

;; --------------------------------------------------------------------------
;; ISR stub address table — used by C code to populate the IDT
;; --------------------------------------------------------------------------

section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dq isr%+i
%assign i i+1
%endrep
