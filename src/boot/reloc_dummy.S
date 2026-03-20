/*
 * Minimal .reloc section stub for non-x86 PE/COFF EFI binaries.
 *
 * On x86_64 the gnu-efi CRT0 supplies this section.  For other
 * architectures we provide a dummy relocation block so that
 * objcopy produces a valid PE image with a BaseRelocationTable.
 */
	.section .reloc, "a"
	.align 2
	.long	0	/* VirtualAddress (dummy) */
	.long	8	/* SizeOfBlock (header-only, zero relocations) */
