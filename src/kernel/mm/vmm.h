#pragma once

#include <aevos/types.h>
#include <aevos/config.h>
#include <aevos/boot_info.h>

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

#define PTE_PRESENT       (1ULL << 0)
#define PTE_WRITABLE      (0ULL)
#define PTE_USER          (1ULL << 6)
#define PTE_WRITE_THROUGH (0ULL)
#define PTE_NO_CACHE      (0ULL)
#define PTE_ACCESSED      (1ULL << 10)
#define PTE_DIRTY         (1ULL << 51)
#define PTE_HUGE          (0ULL)
#define PTE_GLOBAL        (0ULL)
#define PTE_NX            (1ULL << 54)
#define PTE_ADDR_MASK     0x0000FFFFFFFFF000ULL

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
