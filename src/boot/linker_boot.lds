/*
 * UEFI boot stub linker script.
 * Produces a shared object that objcopy converts to a PE/COFF EFI application.
 * Three aligned sections: .text (code), .data (all data), .reloc (PE fixups).
 */

SECTIONS
{
    . = 0;

    .text : {
        _text = .;
        *(.text)
        *(.text.*)
        . = ALIGN(4096);
        _etext = .;
    }

    .data : {
        _data = .;
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
        *(.dynstr)
        *(.dynamic)
        *(.dynsym)
        *(.hash)
        *(.gnu.hash)
        *(.got)
        *(.got.plt)
        *(.rel)
        *(.rel.*)
        *(.rela)
        *(.rela.*)
        . = ALIGN(4096);
        _edata = .;
    }

    .reloc : {
        *(.reloc)
        . = ALIGN(4096);
    }

    .bss : {
        _bss = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
        _ebss = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}

ENTRY(efi_main)
