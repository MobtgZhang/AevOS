/*
 * AevOS Kernel Linker Script — RISC-V 64-bit (Sv48)
 */

OUTPUT_FORMAT("elf64-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(kernel_main)

KERNEL_VBASE = 0xFFFF800000000000;
KERNEL_PBASE = 0x80200000;

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
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VBASE)
    {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(4096);
        _rodata_end = .;
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VBASE)
    {
        _data_start = .;
        *(.data)
        *(.data.*)
        . = ALIGN(4096);
        _data_end = .;
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VBASE)
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
        _bss_end = .;
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.gnu.hash)
        *(.hash)
    }
}
