/*
 * UEFI boot stub linker script.
 * Produces a shared object that objcopy converts to a PE/COFF EFI application.
 * Separate LOAD segments with correct permissions eliminate the RWX warning.
 */

PHDRS
{
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x1000;

    .text : {
        _text = .;
        *(.text)
        *(.text.*)
        . = ALIGN(4096);
        _etext = .;
    } :text

    .data : {
        _data = .;
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
        *(.got)
        *(.got.plt)
        . = ALIGN(4096);
        _edata = .;
    } :data

    .reloc : {
        *(.reloc)
    } :data

    .bss : {
        _bss = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
        _ebss = .;
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.dynstr)
        *(.dynamic)
        *(.dynsym)
        *(.hash)
        *(.gnu.hash)
        *(.rel)
        *(.rel.*)
        *(.rela)
        *(.rela.*)
    }
}

ENTRY(efi_main)
