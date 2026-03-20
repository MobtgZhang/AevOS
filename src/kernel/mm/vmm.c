#include "vmm.h"
#include "pmm.h"
#include "../arch/io.h"
#include "../lib/string.h"

static vmm_ctx_t kernel_ctx;
static bool      vmm_ready;

/* ── Early / late physical-to-virtual ─────────────────── */

static inline uint64_t *ptov(uint64_t paddr) {
    if (vmm_ready)
        return (uint64_t *)(paddr + PHYS_MAP_BASE);
    return (uint64_t *)paddr;       /* identity-mapped during boot */
}

/* ── Helpers ──────────────────────────────────────────── */

static void zero_page(uint64_t paddr) {
    uint64_t *p = ptov(paddr);
    for (int i = 0; i < 512; i++)
        p[i] = 0;
}

static uint64_t alloc_table(void) {
    uint64_t paddr = pmm_alloc_page();
    if (paddr)
        zero_page(paddr);
    return paddr;
}

/*
 * Walk page tables for `vaddr`, optionally creating missing
 * intermediate tables.  Returns a pointer to the final PTE,
 * or NULL on allocation failure.
 */
static uint64_t *vmm_walk(vmm_ctx_t *ctx, uint64_t vaddr, bool create) {
    uint64_t *table = ctx->pml4;

    int indices[3] = {
        (int)PML4_INDEX(vaddr),
        (int)PDPT_INDEX(vaddr),
        (int)PD_INDEX(vaddr),
    };

    for (int level = 0; level < 3; level++) {
        uint64_t entry = table[indices[level]];

        if (entry & PTE_HUGE)
            return &table[indices[level]]; /* 2 MB / 1 GB page */

        if (!(entry & PTE_PRESENT)) {
            if (!create)
                return NULL;

            uint64_t new_table = alloc_table();
            if (!new_table)
                return NULL;

            table[indices[level]] = new_table | PTE_PRESENT
                                              | PTE_WRITABLE
                                              | PTE_USER;
            entry = table[indices[level]];
        }

        table = ptov(entry & PTE_ADDR_MASK);
    }

    /* `table` now points to the PT level */
    return &table[PT_INDEX(vaddr)];
}

/* ── Public API ───────────────────────────────────────── */

void vmm_map_page(vmm_ctx_t *ctx, uint64_t vaddr, uint64_t paddr,
                   uint64_t flags) {
    uint64_t *pte = vmm_walk(ctx, vaddr, true);
    if (pte)
        *pte = (paddr & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK);
}

void vmm_unmap_page(vmm_ctx_t *ctx, uint64_t vaddr) {
    uint64_t *pte = vmm_walk(ctx, vaddr, false);
    if (pte && (*pte & PTE_PRESENT)) {
        *pte = 0;
        invlpg(vaddr);
    }
}

uint64_t vmm_get_phys(vmm_ctx_t *ctx, uint64_t vaddr) {
    uint64_t *pte = vmm_walk(ctx, vaddr, false);
    if (!pte || !(*pte & PTE_PRESENT))
        return 0;

    if (*pte & PTE_HUGE)
        return (*pte & PTE_ADDR_MASK) | (vaddr & 0x1FFFFF);

    return (*pte & PTE_ADDR_MASK) | (vaddr & 0xFFF);
}

void vmm_switch(vmm_ctx_t *ctx) {
    write_cr3(ctx->cr3);
}

/* ── Map a contiguous physical region using 2 MB huge pages ── */

static void map_huge_range(vmm_ctx_t *ctx, uint64_t vbase,
                           uint64_t pbase, uint64_t size,
                           uint64_t flags) {
    uint64_t *pml4 = ctx->pml4;
    uint64_t end = pbase + size;

    for (uint64_t phys = pbase; phys < end; phys += 0x200000) {
        uint64_t vaddr = vbase + (phys - pbase);

        int p4 = (int)PML4_INDEX(vaddr);
        if (!(pml4[p4] & PTE_PRESENT)) {
            uint64_t t = alloc_table();
            pml4[p4] = t | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        uint64_t *pdpt = ptov(pml4[p4] & PTE_ADDR_MASK);

        int p3 = (int)PDPT_INDEX(vaddr);
        if (!(pdpt[p3] & PTE_PRESENT)) {
            uint64_t t = alloc_table();
            pdpt[p3] = t | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        uint64_t *pd = ptov(pdpt[p3] & PTE_ADDR_MASK);

        int p2 = (int)PD_INDEX(vaddr);
        pd[p2] = phys | flags | PTE_HUGE;
    }
}

/* ── Kernel VMM initialization ────────────────────────── */

void vmm_init(boot_info_t *bi) {
    vmm_ready = false;

    uint64_t pml4_phys = alloc_table();
    kernel_ctx.cr3  = pml4_phys;
    kernel_ctx.pml4 = ptov(pml4_phys);

    /*
     * Determine how much physical memory to direct-map.
     * Round up to 2 MB boundary; minimum 4 GB for MMIO space.
     */
    uint64_t map_size = ALIGN_UP(bi->total_memory, 0x200000);
    if (map_size < 4ULL * 1024 * 1024 * 1024)
        map_size = 4ULL * 1024 * 1024 * 1024;

    uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL;

    /* Identity map first 4 GB (physical == virtual) for boot transition */
    map_huge_range(&kernel_ctx, 0, 0,
                   4ULL * 1024 * 1024 * 1024, flags);

    /* Direct physical map at PHYS_MAP_BASE (kernel high-half access) */
    map_huge_range(&kernel_ctx, PHYS_MAP_BASE, 0, map_size, flags);

    /* Map framebuffer at FRAMEBUFFER_VBASE (boot_info may be packed) */
    framebuffer_t fb;
    memcpy(&fb, &bi->fb, sizeof(framebuffer_t));
    if (fb.base) {
        uint64_t fb_phys = (uint64_t)(uintptr_t)fb.base;
        uint64_t fb_size = (uint64_t)fb.pitch * fb.height;
        fb_size = ALIGN_UP(fb_size, 0x200000);
        if (fb_size == 0)
            fb_size = 0x200000;

        map_huge_range(&kernel_ctx, FRAMEBUFFER_VBASE, fb_phys,
                       fb_size, flags | PTE_NO_CACHE);
    }

    /* Activate the new address space */
    vmm_switch(&kernel_ctx);

    /* From this point on, use PHYS_MAP_BASE for all phys->virt access */
    vmm_ready = true;
    kernel_ctx.pml4 = (uint64_t *)(pml4_phys + PHYS_MAP_BASE);
}

/*
 * Create a fresh address-space context for a user agent.
 * Kernel-half PML4 entries (indices 256–511) are shared.
 */
vmm_ctx_t *vmm_create_ctx(void) {
    uint64_t pml4_phys = alloc_table();
    if (!pml4_phys)
        return NULL;

    uint64_t *new_pml4 = PHYS_TO_VIRT(pml4_phys);
    uint64_t *kern_pml4 = kernel_ctx.pml4;

    /* Copy kernel-space mappings */
    for (int i = 256; i < 512; i++)
        new_pml4[i] = kern_pml4[i];

    /* Allocate the context struct from the direct map region */
    uint64_t ctx_phys = pmm_alloc_page();
    if (!ctx_phys) {
        pmm_free_page(pml4_phys);
        return NULL;
    }

    vmm_ctx_t *ctx = PHYS_TO_VIRT(ctx_phys);
    ctx->pml4 = new_pml4;
    ctx->cr3  = pml4_phys;
    return ctx;
}

void vmm_destroy_ctx(vmm_ctx_t *ctx) {
    if (!ctx || ctx == &kernel_ctx)
        return;

    /*
     * Free user-space page tables (indices 0–255).
     * Walk three levels and free each allocated table page.
     */
    uint64_t *pml4 = ctx->pml4;
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_PRESENT))
            continue;
        uint64_t *pdpt = PHYS_TO_VIRT(pml4[i4] & PTE_ADDR_MASK);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE))
                continue;
            uint64_t *pd = PHYS_TO_VIRT(pdpt[i3] & PTE_ADDR_MASK);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE))
                    continue;
                pmm_free_page(pd[i2] & PTE_ADDR_MASK);
            }
            pmm_free_page(pdpt[i3] & PTE_ADDR_MASK);
        }
        pmm_free_page(pml4[i4] & PTE_ADDR_MASK);
    }

    pmm_free_page(ctx->cr3);
    pmm_free_page(VIRT_TO_PHYS((uint64_t)ctx));
}

vmm_ctx_t *vmm_get_kernel_ctx(void) {
    return &kernel_ctx;
}
