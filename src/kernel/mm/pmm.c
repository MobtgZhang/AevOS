#include "pmm.h"

static pmm_t pmm;

/* ── Bitmap helpers ───────────────────────────────────── */

static inline void bm_set(uint64_t page) {
    pmm.bitmap[page / 64] |= (1ULL << (page % 64));
}

static inline void bm_clear(uint64_t page) {
    pmm.bitmap[page / 64] &= ~(1ULL << (page % 64));
}

static inline bool bm_test(uint64_t page) {
    return (pmm.bitmap[page / 64] >> (page % 64)) & 1;
}

static void bm_set_range(uint64_t start, uint64_t count) {
    for (uint64_t i = start; i < start + count && i < PMM_MAX_PAGES; i++)
        bm_set(i);
}

static void bm_clear_range(uint64_t start, uint64_t count) {
    for (uint64_t i = start; i < start + count && i < PMM_MAX_PAGES; i++)
        bm_clear(i);
}

/*
 * Find the first run of `count` contiguous free pages starting
 * from page index `from`.  Returns the index of the first page
 * or UINT64_MAX on failure.
 */
static uint64_t bm_find_free(uint64_t from, uint64_t count) {
    uint64_t run = 0;
    uint64_t start = from;

    for (uint64_t i = from; i < pmm.total_pages; i++) {
        if (bm_test(i)) {
            run = 0;
            start = i + 1;
        } else {
            run++;
            if (run == count)
                return start;
        }
    }
    return UINT64_MAX;
}

/*
 * Fast first-free-page search: scan entire uint64_t words and use
 * __builtin_ctzll on the inverted word to skip fully-used regions.
 */
static uint64_t bm_find_first_free(void) {
    uint64_t words = (pmm.total_pages + 63) / 64;

    for (uint64_t w = 0; w < words; w++) {
        if (pmm.bitmap[w] != UINT64_MAX) {
            uint64_t bit = (uint64_t)__builtin_ctzll(~pmm.bitmap[w]);
            uint64_t page = w * 64 + bit;
            if (page < pmm.total_pages)
                return page;
        }
    }
    return UINT64_MAX;
}

/* ── Public API ───────────────────────────────────────── */

void pmm_init(boot_info_t *bi) {
    /* Start with every page marked as used */
    for (uint64_t i = 0; i < PMM_MAX_PAGES / 64; i++)
        pmm.bitmap[i] = UINT64_MAX;

    pmm.total_pages = bi->total_memory / PAGE_SIZE;
    if (pmm.total_pages > PMM_MAX_PAGES)
        pmm.total_pages = PMM_MAX_PAGES;

    pmm.free_pages = 0;
    pmm.lock = SPINLOCK_INIT;

    /* Walk UEFI memory map and free usable regions */
    for (uint32_t i = 0; i < bi->mmap.count; i++) {
        mmap_entry_t *e = &bi->mmap.entries[i];
        if (e->type != MMAP_USABLE)
            continue;

        uint64_t base  = ALIGN_UP(e->base, PAGE_SIZE);
        uint64_t end   = ALIGN_DOWN(e->base + e->length, PAGE_SIZE);
        if (end <= base)
            continue;

        uint64_t start_page = base / PAGE_SIZE;
        uint64_t num_pages  = (end - base) / PAGE_SIZE;

        if (start_page + num_pages > pmm.total_pages)
            num_pages = pmm.total_pages - start_page;

        bm_clear_range(start_page, num_pages);
        pmm.free_pages += num_pages;
    }

    /* Re-mark kernel physical region as used */
    uint64_t k_start = bi->kernel_phys_base / PAGE_SIZE;
    uint64_t k_pages = ALIGN_UP(bi->kernel_size, PAGE_SIZE) / PAGE_SIZE;
    for (uint64_t i = k_start; i < k_start + k_pages && i < pmm.total_pages; i++) {
        if (!bm_test(i)) {
            bm_set(i);
            pmm.free_pages--;
        }
    }

    /* First 1 MB always reserved (legacy BIOS/firmware area) */
    uint64_t low_pages = (1024 * 1024) / PAGE_SIZE;  /* 256 pages */
    for (uint64_t i = 0; i < low_pages && i < pmm.total_pages; i++) {
        if (!bm_test(i)) {
            bm_set(i);
            pmm.free_pages--;
        }
    }
}

uint64_t pmm_alloc_page(void) {
    spin_lock(&pmm.lock);

    uint64_t page = bm_find_first_free();
    if (page == UINT64_MAX) {
        spin_unlock(&pmm.lock);
        return 0;
    }

    bm_set(page);
    pmm.free_pages--;
    spin_unlock(&pmm.lock);

    return page * PAGE_SIZE;
}

uint64_t pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    spin_lock(&pmm.lock);

    uint64_t start = bm_find_free(0, count);
    if (start == UINT64_MAX) {
        spin_unlock(&pmm.lock);
        return 0;
    }

    bm_set_range(start, count);
    pmm.free_pages -= count;
    spin_unlock(&pmm.lock);

    return start * PAGE_SIZE;
}

void pmm_free_page(uint64_t paddr) {
    uint64_t page = paddr / PAGE_SIZE;
    if (page >= pmm.total_pages) return;

    spin_lock(&pmm.lock);
    if (bm_test(page)) {
        bm_clear(page);
        pmm.free_pages++;
    }
    spin_unlock(&pmm.lock);
}

void pmm_free_pages(uint64_t paddr, size_t count) {
    uint64_t page = paddr / PAGE_SIZE;

    spin_lock(&pmm.lock);
    for (size_t i = 0; i < count; i++) {
        uint64_t p = page + i;
        if (p >= pmm.total_pages) break;
        if (bm_test(p)) {
            bm_clear(p);
            pmm.free_pages++;
        }
    }
    spin_unlock(&pmm.lock);
}

uint64_t pmm_get_free_pages(void) {
    return pmm.free_pages;
}

uint64_t pmm_get_total_pages(void) {
    return pmm.total_pages;
}
