/*
 * AevOS — MIPS64 EL boot shim (kseg0 mode)
 *
 * Creates a synthetic boot_info_t when the kernel is loaded directly
 * by QEMU (-kernel) without UEFI firmware.  Malta board has no
 * standard UEFI; we build a minimal memory map ourselves.
 *
 * All addresses use kseg0 (0xFFFFFFFF80000000+phys) which provides
 * cached, TLB-less access to physical 0x00000000–0x1FFFFFFF.
 */

#include <aevos/types.h>
#include <aevos/config.h>
#include <aevos/boot_info.h>

extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

#define MALTA_RAM_SIZE  (256ULL * 1024 * 1024)

static boot_info_t  g_boot_info;

boot_info_t *mips64_boot_shim(void)
{
    boot_info_t *bi = &g_boot_info;

    uint64_t k_start = (uint64_t)(uintptr_t)_kernel_start;
    uint64_t k_end   = (uint64_t)(uintptr_t)_kernel_end;

    /* Convert kseg0 virtual addresses to physical */
    if (k_start >= PHYS_MAP_BASE)
        k_start -= PHYS_MAP_BASE;
    if (k_end >= PHYS_MAP_BASE)
        k_end -= PHYS_MAP_BASE;

    uint64_t k_size  = k_end - k_start;
    uint64_t k_end_aligned = (k_end + 0xFFF) & ~0xFFFULL;

    uint32_t idx = 0;

    /* Low memory before the kernel (skip first 1 MB for firmware/ISA) */
    if (k_start > 0x100000) {
        bi->mmap.entries[idx].base   = 0x100000;
        bi->mmap.entries[idx].length = k_start - 0x100000;
        bi->mmap.entries[idx].type   = MMAP_USABLE;
        idx++;
    }

    /* Kernel image region */
    bi->mmap.entries[idx].base   = k_start;
    bi->mmap.entries[idx].length = k_size;
    bi->mmap.entries[idx].type   = MMAP_KERNEL;
    idx++;

    /* RAM after the kernel up to 256 MB */
    if (k_end_aligned < MALTA_RAM_SIZE) {
        bi->mmap.entries[idx].base   = k_end_aligned;
        bi->mmap.entries[idx].length = MALTA_RAM_SIZE - k_end_aligned;
        bi->mmap.entries[idx].type   = MMAP_USABLE;
        idx++;
    }

    bi->mmap.count = idx;
    bi->mmap.key   = 0;

    /* No framebuffer from direct boot; virtio-gpu will be set up later */
    bi->fb.base   = NULL;
    bi->fb.width  = 0;
    bi->fb.height = 0;
    bi->fb.pitch  = 0;
    bi->fb.bpp    = 0;

    bi->cfg.model_path[0]  = '\0';
    bi->cfg.n_ctx          = 512;
    bi->cfg.n_threads      = 1;
    bi->cfg.use_gpu        = 0;
    bi->cfg.target_fps     = 30;
    bi->cfg.screen_width   = 1024;
    bi->cfg.screen_height  = 768;

    bi->rsdp             = 0;
    bi->kernel_phys_base = k_start;
    bi->kernel_size      = k_size;
    bi->total_memory     = MALTA_RAM_SIZE;

    bi->magic = BOOT_MAGIC;

    return bi;
}
