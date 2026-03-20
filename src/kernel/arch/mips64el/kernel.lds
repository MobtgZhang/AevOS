/*
 * AevOS Kernel Linker Script — MIPS64 EL
 *
 * Uses kseg0 segment: 0xFFFFFFFF80000000 + phys
 * kseg0 provides direct cached access to physical 0x00000000–0x1FFFFFFF
 * without TLB translations — suitable for QEMU Malta (up to 256 MB).
 */

OUTPUT_FORMAT("elf64-tradlittlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)

KERNEL_VBASE = 0xFFFFFFFF80000000;
KERNEL_PBASE = 0x00200000;

PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    ro   PT_LOAD FLAGS(4);   /* PF_R */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = KERNEL_VBASE + KERNEL_PBASE;
    _kernel_start = .;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VBASE)
    {
        _text_start = .;
        KEEP(*(.text.boot))
        *(.text)
        *(.text.*)
        . = ALIGN(4096);
        _text_end = .;
    } :text

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VBASE)
    {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4096);
        _rodata_end = .;
    } :ro

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VBASE)
    {
        _data_start = .;
        *(.data)
        *(.data.*)
        *(.got)
        *(.got.plt)
        . = ALIGN(4096);
        _data_end = .;
    } :data

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VBASE)
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(.sbss)
        *(.sbss.*)
        *(COMMON)
        . = ALIGN(4096);
        _bss_end = .;
    } :data

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.gnu.hash)
        *(.hash)
        *(.MIPS.abiflags)
        *(.MIPS.options)
        *(.reginfo)
        *(.pdr)
    }
}
