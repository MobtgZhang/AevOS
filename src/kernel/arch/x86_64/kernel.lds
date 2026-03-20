/*
 * AevOS Kernel Linker Script — x86-64 Higher-Half
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(kernel_main)

KERNEL_VBASE = 0xFFFF800000000000;
KERNEL_PBASE = 0x100000;

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
        . = ALIGN(4096);
        _data_end = .;
    } :data

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VBASE)
    {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
        _bss_end = .;
    } :data

    _kernel_end = .;

    .debug_info     0 : { *(.debug_info)     }
    .debug_abbrev   0 : { *(.debug_abbrev)   }
    .debug_line     0 : { *(.debug_line)     }
    .debug_str      0 : { *(.debug_str)      }
    .debug_ranges   0 : { *(.debug_ranges)   }
    .debug_aranges  0 : { *(.debug_aranges)  }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.gnu.hash)
        *(.hash)
    }
}

ASSERT(_bss_start >= _data_end, "BSS must come after .data")
ASSERT(_kernel_end > _kernel_start, "Kernel image must not be empty")
