#pragma once

#include <aevos/types.h>
#include <aevos/config.h>
#include <aevos/boot_info.h>

#define PMM_MAX_PAGES  (1UL << 20)   /* 4 GB addressable via bitmap */

typedef struct {
    uint64_t   bitmap[PMM_MAX_PAGES / 64];
    uint64_t   total_pages;
    uint64_t   free_pages;
    spinlock_t lock;
} pmm_t;

void     pmm_init(boot_info_t *bi);
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages(size_t count);
void     pmm_free_page(uint64_t paddr);
void     pmm_free_pages(uint64_t paddr, size_t count);
uint64_t pmm_get_free_pages(void);
uint64_t pmm_get_total_pages(void);
