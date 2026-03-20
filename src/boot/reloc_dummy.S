/*
 * Minimal PE base relocation section for UEFI PE/COFF on architectures
 * without gnu-efi CRT support (LoongArch64, etc.).
 *
 * UEFI spec requires a valid .reloc data directory entry.
 * A single IMAGE_BASE_RELOCATION block with SizeOfBlock == 8 (header-only,
 * no fixup entries) tells the PE loader "nothing to relocate".  The block
 * must be 4-byte aligned and SizeOfBlock must be a multiple of 4.
 */
	.section .reloc, "a"
	.balign 4
	.long	0		/* VirtualAddress: page RVA */
	.long	8		/* SizeOfBlock: header only, no entries */
