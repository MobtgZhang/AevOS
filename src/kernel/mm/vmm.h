#pragma once

#include <aevos/types.h>
#include <aevos/config.h>
#include <aevos/boot_info.h>

/*
 * L1 地址空间不变量（ideas2）：凡内核约定为「有效」的虚拟地址（含 PHYS_MAP_BASE
 * 线性映射、内核高半部、按需映射的 MMIO），须在页表（或 DMW/xkphys）中可访问。
 * vmm_map_page / vmm_init 的职责即维护该性质。
 */

/* ── Page-table entry flags ──────────────────────────── */

#if defined(__x86_64__)

#define PTE_PRESENT       (1ULL << 0)
#define PTE_WRITABLE      (1ULL << 1)
#define PTE_USER          (1ULL << 2)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_NO_CACHE      (1ULL << 4)
#define PTE_ACCESSED      (1ULL << 5)
#define PTE_DIRTY         (1ULL << 6)
#define PTE_HUGE          (1ULL << 7)
#define PTE_GLOBAL        (1ULL << 8)
#define PTE_NX            (1ULL << 63)
#define PTE_ADDR_MASK     0x000FFFFFFFFFF000ULL

#elif defined(__aarch64__)

/*
 * ARM64 page table descriptors (4KB granule, 4-level):
 *   Table:  bits[1:0] = 0b11   Block (L1/L2): bits[1:0] = 0b01
 *   Page (L3): bits[1:0] = 0b11
 *
 * PTE_WRITABLE is 0 because ARM64 defaults to RW; read-only is
 * indicated by setting AP[2] (bit 7).  The table-type bit (bit 1)
 * is handled by ARM64_TABLE_DESC in architecture-specific code.
 */
#define PTE_PRESENT       (1ULL << 0)
#define PTE_WRITABLE      (0ULL)
#define PTE_USER          (1ULL << 6)
#define PTE_WRITE_THROUGH (0ULL)
#define PTE_NO_CACHE      (1ULL << 2)   /* AttrIndx bit: selects Device attr */
#define PTE_ACCESSED      (1ULL << 10)
#define PTE_DIRTY         (1ULL << 51)
#define PTE_HUGE          (0ULL)
#define PTE_GLOBAL        (0ULL)
#define PTE_NX            (1ULL << 54)
#define PTE_ADDR_MASK     0x0000FFFFFFFFF000ULL

/* ARM64-specific descriptor helpers (used by vmm.c and boot) */
#define ARM64_TABLE_DESC    (3ULL)             /* Valid + Table */
#define ARM64_BLOCK_DESC    (1ULL)             /* Valid + Block */
#define ARM64_ATTR_IDX(n)   ((uint64_t)(n) << 2)
#define ARM64_SH_INNER      (3ULL << 8)
#define ARM64_AF             (1ULL << 10)
#define ARM64_BLOCK_NORMAL  (ARM64_BLOCK_DESC | ARM64_ATTR_IDX(2) | \
                             ARM64_SH_INNER | ARM64_AF)
#define ARM64_BLOCK_DEVICE  (ARM64_BLOCK_DESC | ARM64_ATTR_IDX(0) | \
                             ARM64_SH_INNER | ARM64_AF)

#elif defined(__riscv)

#define PTE_PRESENT       (1ULL << 0)
#define PTE_WRITABLE      (1ULL << 2)
#define PTE_USER          (1ULL << 4)
#define PTE_WRITE_THROUGH (0ULL)
#define PTE_NO_CACHE      (0ULL)
#define PTE_ACCESSED      (1ULL << 6)
#define PTE_DIRTY         (1ULL << 7)
#define PTE_HUGE          (0ULL)
#define PTE_GLOBAL        (1ULL << 5)
#define PTE_NX            (0ULL)
#define PTE_ADDR_MASK     0x003FFFFFFFFFFC00ULL

#elif defined(__loongarch64)

#define PTE_PRESENT       (1ULL << 0)
#define PTE_WRITABLE      (1ULL << 1)
#define PTE_USER          (1ULL << 2)
#define PTE_WRITE_THROUGH (0ULL)
#define PTE_NO_CACHE      (1ULL << 4)
#define PTE_ACCESSED      (0ULL)
#define PTE_DIRTY         (1ULL << 6)
#define PTE_HUGE          (1ULL << 7)
#define PTE_GLOBAL        (1ULL << 8)
#define PTE_NX            (1ULL << 63)
#define PTE_ADDR_MASK     0x00FFFFFFFFFFF000ULL

#else

#define PTE_PRESENT       (1ULL << 0)
#define PTE_WRITABLE      (1ULL << 1)
#define PTE_USER          (1ULL << 2)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_NO_CACHE      (1ULL << 4)
#define PTE_ACCESSED      (1ULL << 5)
#define PTE_DIRTY         (1ULL << 6)
#define PTE_HUGE          (1ULL << 7)
#define PTE_GLOBAL        (1ULL << 8)
#define PTE_NX            (1ULL << 63)
#define PTE_ADDR_MASK     0x000FFFFFFFFFF000ULL

#endif

/* Common flag combinations */
#define PTE_KERN_RW       (PTE_PRESENT | PTE_WRITABLE)
#define PTE_KERN_RO       (PTE_PRESENT)
#define PTE_USER_RW       (PTE_PRESENT | PTE_WRITABLE | PTE_USER)
#define PTE_USER_RO       (PTE_PRESENT | PTE_USER)

/* Page table index helpers (same for 4-level 48-bit VA) */
#define PML4_INDEX(addr)  (((uint64_t)(addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr)  (((uint64_t)(addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)    (((uint64_t)(addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)    (((uint64_t)(addr) >> 12) & 0x1FF)

/* Physical / virtual address conversion */
#define PHYS_TO_VIRT(paddr) ((void *)((uint64_t)(paddr) + PHYS_MAP_BASE))
#define VIRT_TO_PHYS(vaddr) ((uint64_t)(vaddr) - PHYS_MAP_BASE)

/* Address-space context */
typedef struct {
    uint64_t *pml4;     /* virtual address of root page table */
    uint64_t  cr3;      /* physical address of root page table */
} vmm_ctx_t;

void       vmm_init(boot_info_t *bi);
vmm_ctx_t *vmm_create_ctx(void);
void       vmm_destroy_ctx(vmm_ctx_t *ctx);

void       vmm_map_page(vmm_ctx_t *ctx, uint64_t vaddr,
                         uint64_t paddr, uint64_t flags);
void       vmm_unmap_page(vmm_ctx_t *ctx, uint64_t vaddr);
uint64_t   vmm_get_phys(vmm_ctx_t *ctx, uint64_t vaddr);
void       vmm_switch(vmm_ctx_t *ctx);

vmm_ctx_t *vmm_get_kernel_ctx(void);
