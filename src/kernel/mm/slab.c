#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include <aevos/config.h>

/* ── Helpers (no libc in freestanding kernel) ─────────── */

static void *kmemcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static void *kmemset(void *dst, int val, size_t n) {
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)val;
    return dst;
}

/* ── Slab data structures ─────────────────────────────── */

#define SLAB_NUM_CACHES   12
#define SLAB_POOL_SIZE    512
#define SLAB_MAX_OBJECTS  128  /* max objects per slab (for 32-byte cache) */

static const size_t cache_sizes[SLAB_NUM_CACHES] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
};

typedef struct slab {
    void                *base;        /* virtual address of data pages */
    uint64_t             phys;        /* physical address              */
    uint32_t             total_obj;
    uint32_t             free_count;
    uint64_t             bitmap[2];   /* up to 128 objects (2×64 bits) */
    struct slab         *next;
    struct slab_cache   *cache;
} slab_t;

typedef struct slab_cache {
    size_t      obj_size;
    uint32_t    pages_per_slab;
    uint32_t    obj_per_slab;
    slab_t     *partial;      /* at least one free object */
    slab_t     *full;         /* no free objects          */
    spinlock_t  lock;
} slab_cache_t;

static slab_cache_t caches[SLAB_NUM_CACHES];
static slab_t       slab_pool[SLAB_POOL_SIZE];
static uint32_t     slab_pool_next;
static spinlock_t   pool_lock;

/* ── Bitmap operations on the 128-bit slab bitmap ─────── */

static inline void slab_bm_set(slab_t *s, uint32_t idx) {
    s->bitmap[idx / 64] |= (1ULL << (idx % 64));
}

static inline void slab_bm_clear(slab_t *s, uint32_t idx) {
    s->bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

static inline bool slab_bm_test(slab_t *s, uint32_t idx) {
    return (s->bitmap[idx / 64] >> (idx % 64)) & 1;
}

/*
 * Find the first zero bit in the slab bitmap.
 * Returns the bit index, or UINT32_MAX if none free.
 */
static uint32_t slab_bm_find_free(slab_t *s) {
    uint32_t words = (s->total_obj + 63) / 64;
    for (uint32_t w = 0; w < words; w++) {
        if (s->bitmap[w] == UINT64_MAX)
            continue;
        uint32_t bit = (uint32_t)__builtin_ctzll(~s->bitmap[w]);
        uint32_t idx = w * 64 + bit;
        if (idx < s->total_obj)
            return idx;
    }
    return UINT32_MAX;
}

/* ── Slab pool allocator ──────────────────────────────── */

static slab_t *slab_pool_alloc(void) {
    spin_lock(&pool_lock);
    slab_t *s = NULL;
    if (slab_pool_next < SLAB_POOL_SIZE) {
        s = &slab_pool[slab_pool_next++];
    }
    spin_unlock(&pool_lock);
    return s;
}

/* ── Create a new slab for a given cache ──────────────── */

static slab_t *slab_create(slab_cache_t *cache) {
    slab_t *s = slab_pool_alloc();
    if (!s)
        return NULL;

    s->phys = pmm_alloc_pages(cache->pages_per_slab);
    if (!s->phys)
        return NULL;

    s->base       = PHYS_TO_VIRT(s->phys);
    s->total_obj  = cache->obj_per_slab;
    s->free_count = cache->obj_per_slab;
    s->bitmap[0]  = 0;
    s->bitmap[1]  = 0;
    s->next       = NULL;
    s->cache      = cache;

    kmemset(s->base, 0, (size_t)cache->pages_per_slab * PAGE_SIZE);
    return s;
}

/* ── Remove a slab from a singly-linked list ──────────── */

static void slab_list_remove(slab_t **head, slab_t *target) {
    slab_t **pp = head;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            target->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ── Allocate one object from a specific cache ────────── */

static void *cache_alloc(slab_cache_t *cache) {
    spin_lock(&cache->lock);

    /* Try to allocate from a partial slab */
    slab_t *s = cache->partial;
    if (!s) {
        /* No partial slabs — create a new one */
        s = slab_create(cache);
        if (!s) {
            spin_unlock(&cache->lock);
            return NULL;
        }
        s->next = cache->partial;
        cache->partial = s;
    }

    uint32_t idx = slab_bm_find_free(s);
    if (idx == UINT32_MAX) {
        /* Should not happen for a partial slab */
        spin_unlock(&cache->lock);
        return NULL;
    }

    slab_bm_set(s, idx);
    s->free_count--;

    void *obj = (uint8_t *)s->base + (size_t)idx * cache->obj_size;

    /* If slab is now full, move it to the full list */
    if (s->free_count == 0) {
        slab_list_remove(&cache->partial, s);
        s->next = cache->full;
        cache->full = s;
    }

    spin_unlock(&cache->lock);
    return obj;
}

/* ── Free one object back to its cache ────────────────── */

static void cache_free(slab_cache_t *cache, void *ptr) {
    spin_lock(&cache->lock);

    /*
     * Find the slab that owns this pointer by scanning
     * partial and full lists.  O(N) per call — adequate for
     * the current scale; can be replaced with a page-indexed
     * lookup table if needed.
     */
    size_t slab_bytes = (size_t)cache->pages_per_slab * PAGE_SIZE;
    slab_t *s = NULL;
    bool was_full = false;

    for (slab_t *p = cache->partial; p; p = p->next) {
        if ((uint8_t *)ptr >= (uint8_t *)p->base &&
            (uint8_t *)ptr <  (uint8_t *)p->base + slab_bytes) {
            s = p;
            break;
        }
    }
    if (!s) {
        for (slab_t *p = cache->full; p; p = p->next) {
            if ((uint8_t *)ptr >= (uint8_t *)p->base &&
                (uint8_t *)ptr <  (uint8_t *)p->base + slab_bytes) {
                s = p;
                was_full = true;
                break;
            }
        }
    }

    if (!s) {
        spin_unlock(&cache->lock);
        return;  /* double-free or invalid pointer */
    }

    uint32_t idx = (uint32_t)((uint64_t)ptr - (uint64_t)s->base)
                   / (uint32_t)cache->obj_size;

    if (!slab_bm_test(s, idx)) {
        spin_unlock(&cache->lock);
        return;  /* already free */
    }

    slab_bm_clear(s, idx);
    s->free_count++;

    /* Move from full → partial if it was previously full */
    if (was_full) {
        slab_list_remove(&cache->full, s);
        s->next = cache->partial;
        cache->partial = s;
    }

    /* If slab is completely empty, optionally reclaim it. For now keep it
       in the partial list so the next allocation is fast. */

    spin_unlock(&cache->lock);
}

/* ── Find the smallest cache that fits `size` ─────────── */

static slab_cache_t *find_cache(size_t size) {
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        if (size <= caches[i].obj_size)
            return &caches[i];
    }
    return NULL;
}

/*
 * Walk all caches to find which one owns `ptr`.
 * Returns NULL if the pointer does not belong to any cache.
 */
static slab_cache_t *find_cache_for_ptr(void *ptr, uint32_t *out_idx,
                                        slab_t **out_slab) {
    for (int c = 0; c < SLAB_NUM_CACHES; c++) {
        slab_cache_t *cache = &caches[c];
        size_t slab_bytes = (size_t)cache->pages_per_slab * PAGE_SIZE;

        for (slab_t *s = cache->partial; s; s = s->next) {
            if ((uint8_t *)ptr >= (uint8_t *)s->base &&
                (uint8_t *)ptr <  (uint8_t *)s->base + slab_bytes) {
                if (out_slab) *out_slab = s;
                if (out_idx)
                    *out_idx = (uint32_t)((uint64_t)ptr - (uint64_t)s->base)
                               / (uint32_t)cache->obj_size;
                return cache;
            }
        }
        for (slab_t *s = cache->full; s; s = s->next) {
            if ((uint8_t *)ptr >= (uint8_t *)s->base &&
                (uint8_t *)ptr <  (uint8_t *)s->base + slab_bytes) {
                if (out_slab) *out_slab = s;
                if (out_idx)
                    *out_idx = (uint32_t)((uint64_t)ptr - (uint64_t)s->base)
                               / (uint32_t)cache->obj_size;
                return cache;
            }
        }
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────── */

void slab_init(void) {
    slab_pool_next = 0;
    pool_lock      = SPINLOCK_INIT;

    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        caches[i].obj_size       = cache_sizes[i];
        caches[i].pages_per_slab = (cache_sizes[i] <= PAGE_SIZE)
                                       ? 1
                                       : (uint32_t)((cache_sizes[i] + PAGE_SIZE - 1) / PAGE_SIZE);
        caches[i].obj_per_slab   = (uint32_t)((caches[i].pages_per_slab * PAGE_SIZE)
                                               / cache_sizes[i]);
        if (caches[i].obj_per_slab > SLAB_MAX_OBJECTS)
            caches[i].obj_per_slab = SLAB_MAX_OBJECTS;

        caches[i].partial = NULL;
        caches[i].full    = NULL;
        caches[i].lock    = SPINLOCK_INIT;
    }
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;

    slab_cache_t *cache = find_cache(size);
    if (!cache)
        return NULL;   /* request exceeds SLAB_MAX_SIZE */

    return cache_alloc(cache);
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    slab_cache_t *cache = find_cache_for_ptr(ptr, NULL, NULL);
    if (cache)
        cache_free(cache, ptr);
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr)
        return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    /* Determine old object size so we know how much to copy */
    slab_cache_t *old_cache = find_cache_for_ptr(ptr, NULL, NULL);
    if (!old_cache)
        return NULL;

    /* If the current cache already satisfies new_size, no-op */
    if (new_size <= old_cache->obj_size)
        return ptr;

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr)
        return NULL;

    kmemcpy(new_ptr, ptr, old_cache->obj_size);
    kfree(ptr);
    return new_ptr;
}

void *kcalloc(size_t count, size_t size) {
    size_t total = count * size;
    if (count != 0 && total / count != size)
        return NULL;  /* overflow */

    void *ptr = kmalloc(total);
    if (ptr)
        kmemset(ptr, 0, total);
    return ptr;
}
