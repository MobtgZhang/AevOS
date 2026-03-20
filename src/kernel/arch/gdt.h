#pragma once

/*
 * GDT initialization.
 * On x86-64: sets up Global Descriptor Table with kernel/user segments + TSS.
 * On other architectures: no-op (no GDT concept).
 */
void gdt_init(void);
