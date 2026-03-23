#pragma once

#include <aevos/types.h>

/*
 * HMS semantic cache tiers (L1 hot / L2 warm / L3 cold).
 * CLOCK-Pro–style eviction for L1; L2/L3 are stubs for SQLite + mmap paths.
 */
typedef struct {
    uint32_t l1_slots;
    uint32_t l1_used;
    uint64_t hits;
    uint64_t misses;
    void    *l1_private;
    spinlock_t lock;
} hms_cache_t;

void hms_cache_init(hms_cache_t *c, uint32_t l1_slots);
void hms_cache_destroy(hms_cache_t *c);

/* Session / KV hot path */
int  hms_cache_l1_get(hms_cache_t *c, const char *key, void *out, size_t *io_len);
int  hms_cache_l1_put(hms_cache_t *c, const char *key, const void *data, size_t len);
