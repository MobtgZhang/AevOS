#include "vmm.h"
#include "pmm.h"
#include "../drivers/pci.h"
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

#if defined(__riscv)
/* Sv48：非叶仅 V；叶须 V+R+W+X（与 efi_main RV_PTE_* 一致） */
#define RV_PTE_V  (1ULL << 0)
#define RV_PTE_R  (1ULL << 1)
#define RV_PTE_W  (1ULL << 2)
#define RV_PTE_X  (1ULL << 3)
#define RV_PTE_U  (1ULL << 4)
#define RV_PTE_G  (1ULL << 5)
#define RV_PTE_A  (1ULL << 6)
#define RV_PTE_D  (1ULL << 7)

static inline bool rv_pte_is_leaf(uint64_t pte)
{
    return (pte & RV_PTE_V) && (pte & (RV_PTE_R | RV_PTE_W | RV_PTE_X));
}

static inline uint64_t rv_pte_table_phys(uint64_t pte)
{
    return (pte >> 10) << 12;
}

static inline uint64_t rv_make_table_pte(uint64_t table_phys)
{
    return ((table_phys >> 12) << 10) | RV_PTE_V;
}

static inline uint64_t rv_make_leaf_pte_2m(uint64_t phys, uint64_t flags, bool with_exec)
{
    uint64_t a = ((phys >> 12) << 10) | RV_PTE_V | RV_PTE_R | RV_PTE_W | RV_PTE_A | RV_PTE_D;
    /*
     * MMIO 叶项勿设 G：部分核上 Global+设备叶与 I/O 访问组合会异常；可执行 RAM 再带 G。
     */
    if (with_exec)
        a |= RV_PTE_X | RV_PTE_G;
    if (flags & PTE_USER)
        a |= RV_PTE_U;
    return a;
}
#endif /* __riscv */

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

#if defined(__aarch64__)
        /* ARM64 block descriptor: valid=1, table=0 → bits[1:0]=01 */
        if ((entry & 3) == 1)
            return &table[indices[level]];
#elif defined(__riscv)
        if (rv_pte_is_leaf(entry))
            return &table[indices[level]];
#else
        if (entry & PTE_HUGE)
            return &table[indices[level]]; /* 2 MB / 1 GB page */
#endif

        if (!(entry & PTE_PRESENT)) {
            if (!create)
                return NULL;

            uint64_t new_table = alloc_table();
            if (!new_table)
                return NULL;

#if defined(__aarch64__)
            table[indices[level]] = new_table | ARM64_TABLE_DESC;
#elif defined(__riscv)
            table[indices[level]] = rv_make_table_pte(new_table);
#else
            table[indices[level]] = new_table | PTE_PRESENT
                                              | PTE_WRITABLE
                                              | PTE_USER;
#endif
            entry = table[indices[level]];
        }

#if defined(__riscv)
        table = ptov(rv_pte_table_phys(entry));
#else
        table = ptov(entry & PTE_ADDR_MASK);
#endif
    }

    /* `table` now points to the PT level */
    return &table[PT_INDEX(vaddr)];
}

/* ── Public API ───────────────────────────────────────── */

void vmm_map_page(vmm_ctx_t *ctx, uint64_t vaddr, uint64_t paddr,
                   uint64_t flags) {
    uint64_t *pte = vmm_walk(ctx, vaddr, true);
    if (!pte)
        return;
#if defined(__riscv)
    (void)vaddr;
    *pte = ((paddr >> 12) << 10) | RV_PTE_V | RV_PTE_R | RV_PTE_W | RV_PTE_X
           | RV_PTE_G | RV_PTE_A | RV_PTE_D | ((flags & PTE_USER) ? RV_PTE_U : 0);
#else
    *pte = (paddr & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK);
#endif
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

#if defined(__aarch64__)
    if ((*pte & 3) == 1)  /* block (2 MiB) */
        return (*pte & PTE_ADDR_MASK) | (vaddr & 0x1FFFFF);
    if ((*pte & 3) == 3)  /* page (4 KiB) */
        return (*pte & PTE_ADDR_MASK) | (vaddr & 0xFFF);
    return 0;
#elif defined(__riscv)
    if (rv_pte_is_leaf(*pte))
        return rv_pte_table_phys(*pte) | (vaddr & 0x1FFFFF);
    return rv_pte_table_phys(*pte) | (vaddr & 0xFFF);
#else
    if (*pte & PTE_HUGE)
        return (*pte & PTE_ADDR_MASK) | (vaddr & 0x1FFFFF);

    return (*pte & PTE_ADDR_MASK) | (vaddr & 0xFFF);
#endif
}

void vmm_switch(vmm_ctx_t *ctx) {
    write_cr3(ctx->cr3);
}

/* ── Map a contiguous physical region using 2 MB huge pages ── */

#if !defined(__loongarch64)
static void map_huge_range(vmm_ctx_t *ctx, uint64_t vbase,
                           uint64_t pbase, uint64_t size,
                           uint64_t flags, bool riscv_leaf_exec) {
    uint64_t *pml4 = ctx->pml4;
    uint64_t end = pbase + size;

    for (uint64_t phys = pbase; phys < end; phys += 0x200000) {
        uint64_t vaddr = vbase + (phys - pbase);

        int p4 = (int)PML4_INDEX(vaddr);
        if (!(pml4[p4] & PTE_PRESENT)) {
            uint64_t t = alloc_table();
#if defined(__aarch64__)
            pml4[p4] = t | ARM64_TABLE_DESC;
#elif defined(__riscv)
            pml4[p4] = rv_make_table_pte(t);
#else
            pml4[p4] = t | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
#endif
        }
#if defined(__riscv)
        uint64_t *pdpt = ptov(rv_pte_table_phys(pml4[p4]));
#else
        uint64_t *pdpt = ptov(pml4[p4] & PTE_ADDR_MASK);
#endif

        int p3 = (int)PDPT_INDEX(vaddr);
        if (!(pdpt[p3] & PTE_PRESENT)) {
            uint64_t t = alloc_table();
#if defined(__aarch64__)
            pdpt[p3] = t | ARM64_TABLE_DESC;
#elif defined(__riscv)
            pdpt[p3] = rv_make_table_pte(t);
#else
            pdpt[p3] = t | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
#endif
        }
#if defined(__riscv)
        uint64_t *pd = ptov(rv_pte_table_phys(pdpt[p3]));
#else
        uint64_t *pd = ptov(pdpt[p3] & PTE_ADDR_MASK);
#endif

        int p2 = (int)PD_INDEX(vaddr);
#if defined(__aarch64__)
        (void)riscv_leaf_exec;
        pd[p2] = phys | ((flags & PTE_NO_CACHE) ? ARM64_BLOCK_DEVICE
                                                 : ARM64_BLOCK_NORMAL);
#elif defined(__riscv)
        pd[p2] = rv_make_leaf_pte_2m(phys, flags, riscv_leaf_exec);
#else
        (void)riscv_leaf_exec;
        pd[p2] = phys | flags | PTE_HUGE;
#endif
    }
}
#endif /* !__loongarch64 */

/* ── Kernel VMM initialization ────────────────────────── */

void vmm_init(boot_info_t *bi) {
    vmm_ready = false;

#if defined(__loongarch64)
    /*
     * LoongArch uses DMW (Direct Mapping Windows): hardware-level
     * physical-to-virtual translation without software page tables.
     */
    (void)bi;
    kernel_ctx.cr3  = 0;
    kernel_ctx.pml4 = NULL;
    vmm_ready = true;
#else
    uint64_t pml4_phys = alloc_table();
    kernel_ctx.cr3  = pml4_phys;
    kernel_ctx.pml4 = ptov(pml4_phys);

    uint64_t map_size = ALIGN_UP(bi->total_memory, 0x200000);
    if (map_size < 4ULL * 1024 * 1024 * 1024)
        map_size = 4ULL * 1024 * 1024 * 1024;

    uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL;

#if defined(__aarch64__)
    /*
     * AArch64 uses split TTBR0/TTBR1.  The bootloader already set up
     * TTBR0 (identity map) which remains active.  Here we only build
     * the TTBR1 root for higher-half kernel mappings.
     */
#else
    /* Identity map first 4 GB (physical == virtual) for boot transition */
    map_huge_range(&kernel_ctx, 0, 0,
                   4ULL * 1024 * 1024 * 1024, flags, true);
#endif

    /* Direct physical map at PHYS_MAP_BASE (kernel high-half access) */
#if defined(__aarch64__)
    /*
     * QEMU virt: first 1 GiB is device MMIO (PCI BARs, UART, GIC…),
     * must be mapped with Device-nGnRnE attributes (PTE_NO_CACHE).
     */
    map_huge_range(&kernel_ctx, PHYS_MAP_BASE, 0,
                   1ULL * 1024 * 1024 * 1024,
                   flags | PTE_NO_CACHE, true);
    map_huge_range(&kernel_ctx,
                   PHYS_MAP_BASE + 1ULL * 1024 * 1024 * 1024,
                   1ULL * 1024 * 1024 * 1024,
                   map_size - 1ULL * 1024 * 1024 * 1024,
                   flags, true);

    /*
     * PCI ECAM: QEMU virt (highmem=on) places ECAM at 0x4010000000.
     * Map as Device memory so PCI config-space reads work correctly.
     */
#ifdef PCI_ECAM_PHYS
    map_huge_range(&kernel_ctx,
                   PHYS_MAP_BASE + PCI_ECAM_PHYS,
                   PCI_ECAM_PHYS,
                   ALIGN_UP(PCI_ECAM_SIZE, 0x200000),
                   flags | PTE_NO_CACHE, true);
#endif
    /*
     * 64-bit PCI MMIO window for BARs (e.g. virtio-gpu config).
     */
#ifdef PCI_MMIO64_PHYS
    map_huge_range(&kernel_ctx,
                   PHYS_MAP_BASE + PCI_MMIO64_PHYS,
                   PCI_MMIO64_PHYS,
                   ALIGN_UP(PCI_MMIO64_SIZE, 0x200000),
                   flags | PTE_NO_CACHE, true);
#endif
#else
    map_huge_range(&kernel_ctx, PHYS_MAP_BASE, 0, map_size, flags, true);
#if defined(__x86_64__)
    /*
     * QEMU / PC 固件常把 64-bit PCI BAR（virtio 1.0 modern）放在物理地址 ≥4G
     *（例如 0xC000000000）。低 4G 身份映射访问不到，必须在 PHYS_MAP_BASE
     * 下映射一片高 MMIO 窗口（与 pci_bar_to_mmio_vaddr 一致）。
     */
    map_huge_range(&kernel_ctx,
                   PHYS_MAP_BASE + AEVOS_X86_PCI_MMIO64_PHYS_LO,
                   AEVOS_X86_PCI_MMIO64_PHYS_LO,
                   AEVOS_X86_PCI_MMIO64_PHYS_SZ,
                   flags | PTE_NO_CACHE, true);
#endif
#if defined(PCI_MMIO64_PHYS) && defined(PCI_MMIO64_SIZE)
    /*
     * RISC-V virt 等：virtio-pci modern 的 64-bit BAR 落在 RAM 以上的高 MMIO，
     * 不在 [0, map_size) 内，须单独映射（AArch64 在同文件上方分支已处理）。
     */
    map_huge_range(&kernel_ctx,
                   PHYS_MAP_BASE + PCI_MMIO64_PHYS,
                   PCI_MMIO64_PHYS,
                   ALIGN_UP(PCI_MMIO64_SIZE, 0x200000),
                   flags | PTE_NO_CACHE, true);
#endif
#endif

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
                       fb_size, flags | PTE_NO_CACHE, true);
    }

    /* Activate the new address space */
#if defined(__aarch64__)
    /*
     * Build a kernel-owned TTBR0 identity map so the stack (which uses
     * low addresses via TTBR0) survives after the PMM reclaims the
     * bootloader's page table pages.  Uses L1 1 GiB block descriptors
     * for simplicity — only 2 pages needed.
     */
    {
        uint64_t ttbr0_l0_phys = alloc_table();
        uint64_t ttbr0_l1_phys = alloc_table();
        uint64_t *l0_id = ptov(ttbr0_l0_phys);
        uint64_t *l1_id = ptov(ttbr0_l1_phys);

        l0_id[0] = ttbr0_l1_phys | ARM64_TABLE_DESC;

        uint64_t id_map_gib = ALIGN_UP(bi->total_memory, 1ULL << 30) >> 30;
        if (id_map_gib < 8) id_map_gib = 8;
        if (id_map_gib > 512) id_map_gib = 512;

        for (uint64_t i = 0; i < id_map_gib; i++) {
            uint64_t phys = i << 30;
            l1_id[i] = phys | ((phys < 0x40000000ULL)
                               ? ARM64_BLOCK_DEVICE : ARM64_BLOCK_NORMAL);
        }

        __asm__ volatile(
            "msr ttbr0_el1, %0\n\t"
            "msr ttbr1_el1, %1\n\t"
            "isb\n\t"
            "tlbi vmalle1\n\t"
            "dsb sy\n\t"
            "isb"
            : : "r"(ttbr0_l0_phys), "r"(pml4_phys) : "memory"
        );
    }
#else
    vmm_switch(&kernel_ctx);
#endif

    /* From this point on, use PHYS_MAP_BASE for all phys->virt access */
    vmm_ready = true;
    kernel_ctx.pml4 = (uint64_t *)(pml4_phys + PHYS_MAP_BASE);
#endif
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
#if defined(__aarch64__)
        if ((pml4[i4] & 3) == 1) continue;   /* skip block descriptors */
#endif
#if defined(__riscv)
        if (rv_pte_is_leaf(pml4[i4]))
            continue;
        uint64_t *pdpt = PHYS_TO_VIRT(rv_pte_table_phys(pml4[i4]));
#else
        uint64_t *pdpt = PHYS_TO_VIRT(pml4[i4] & PTE_ADDR_MASK);
#endif
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT))
                continue;
#if defined(__aarch64__)
            if ((pdpt[i3] & 3) == 1) continue;
#elif defined(__riscv)
            if (rv_pte_is_leaf(pdpt[i3]))
                continue;
#else
            if (pdpt[i3] & PTE_HUGE) continue;
#endif
#if defined(__riscv)
            uint64_t *pd = PHYS_TO_VIRT(rv_pte_table_phys(pdpt[i3]));
#else
            uint64_t *pd = PHYS_TO_VIRT(pdpt[i3] & PTE_ADDR_MASK);
#endif
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT))
                    continue;
#if defined(__aarch64__)
                if ((pd[i2] & 3) == 1) continue;
#elif defined(__riscv)
                if (rv_pte_is_leaf(pd[i2]))
                    continue;
#else
                if (pd[i2] & PTE_HUGE) continue;
#endif
#if defined(__riscv)
                pmm_free_page(rv_pte_table_phys(pd[i2]));
#else
                pmm_free_page(pd[i2] & PTE_ADDR_MASK);
#endif
            }
#if defined(__riscv)
            pmm_free_page(rv_pte_table_phys(pdpt[i3]));
#else
            pmm_free_page(pdpt[i3] & PTE_ADDR_MASK);
#endif
        }
#if defined(__riscv)
        pmm_free_page(rv_pte_table_phys(pml4[i4]));
#else
        pmm_free_page(pml4[i4] & PTE_ADDR_MASK);
#endif
    }

    pmm_free_page(ctx->cr3);
    pmm_free_page(VIRT_TO_PHYS((uint64_t)ctx));
}

vmm_ctx_t *vmm_get_kernel_ctx(void) {
    return &kernel_ctx;
}
