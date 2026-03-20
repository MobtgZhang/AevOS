#pragma once

#include <aevos/types.h>

/*
 * AevOS Vector Index — FAISS-inspired quantized vector search for RAG.
 *
 * Uses int8_t quantized embeddings (no floating point required).
 * Supports flat (brute-force) and IVF (inverted-file) index types.
 */

typedef enum {
    VECINDEX_FLAT = 0,
    VECINDEX_IVF  = 1,
} vecindex_type_t;

typedef enum {
    VECDIST_L2 = 0,    /* squared L2 distance (lower = closer) */
    VECDIST_IP = 1,     /* inner product (higher = closer) */
} vecindex_dist_t;

typedef struct vecindex vecindex_t;

/* ── Lifecycle ──────────────────────────────────────────── */

vecindex_t *vecindex_create(uint32_t dim, vecindex_type_t type,
                            vecindex_dist_t dist);
void        vecindex_destroy(vecindex_t *idx);

/* ── Adding vectors ─────────────────────────────────────── */

int vecindex_add(vecindex_t *idx, uint64_t id, const int8_t *vec);
int vecindex_add_batch(vecindex_t *idx, const uint64_t *ids,
                       const int8_t *vecs, uint32_t count);

/* ── Search ─────────────────────────────────────────────── */

int vecindex_search(vecindex_t *idx, const int8_t *query, uint32_t k,
                    uint64_t *out_ids, int32_t *out_dists);

/* ── Management ─────────────────────────────────────────── */

int      vecindex_remove(vecindex_t *idx, uint64_t id);
uint32_t vecindex_count(vecindex_t *idx);

/* ── IVF training ───────────────────────────────────────── */

int vecindex_train(vecindex_t *idx, const int8_t *vecs, uint32_t count);

/* ── Serialization ──────────────────────────────────────── */

ssize_t     vecindex_serialize(vecindex_t *idx, void *buf, size_t size);
vecindex_t *vecindex_deserialize(const void *buf, size_t size);
