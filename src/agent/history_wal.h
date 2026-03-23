#pragma once

#include <aevos/types.h>

/* Append-only WAL for history crash consistency (metadata + length-prefixed blobs). */
typedef struct {
    uint64_t base_seq;
    spinlock_t lock;
} history_wal_t;

void history_wal_init(history_wal_t *w);
int  history_wal_append(history_wal_t *w, uint8_t role,
                        const void *data, uint32_t len);
