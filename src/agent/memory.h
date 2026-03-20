#pragma once

#include <aevos/types.h>
#include <aevos/config.h>
#include "lib/hashmap.h"

#define HNSW_M            16
#define HNSW_M0           32
#define HNSW_EF_CONSTRUCT 200
#define HNSW_EF_SEARCH    50
#define HNSW_MAX_LEVEL    6

typedef struct {
    int8_t    embedding[EMBED_DIM];
    float     importance;
    uint64_t  last_access;
    uint32_t  access_count;
    uint8_t  *content;
    uint32_t  content_len;
    uint64_t  id;
} mem_entry_t;

typedef struct {
    uint32_t  id;
    uint8_t   level;
    uint32_t  neighbors[HNSW_MAX_LEVEL][HNSW_M0];
    uint8_t   n_neighbors[HNSW_MAX_LEVEL];
} hnsw_node_t;

typedef struct {
    hnsw_node_t *nodes;
    uint32_t     count;
    uint32_t     capacity;
    int          max_level;
    uint32_t     entry_point;
    float        level_mult;
} hnsw_graph_t;

typedef struct {
    uint64_t id;
    float    score;
} mem_result_t;

typedef struct {
    mem_entry_t   *entries;
    uint32_t       count;
    uint32_t       capacity;
    uint64_t       next_id;
    hnsw_graph_t  *index;
    hashmap_t     *working_mem;
    spinlock_t     lock;
} memory_engine_t;

int  memory_init(memory_engine_t *engine);
void memory_destroy(memory_engine_t *engine);

uint64_t memory_store(memory_engine_t *engine, const void *content,
                      size_t content_len, const int8_t *embedding,
                      float importance);
int      memory_retrieve(memory_engine_t *engine, const int8_t *query_embedding,
                         uint32_t top_k, mem_result_t *results);
void     memory_update_importance(memory_engine_t *engine, uint64_t id,
                                  float new_importance);

void memory_forget(memory_engine_t *engine);

void  memory_working_set(memory_engine_t *engine, const char *key, void *value);
void *memory_working_get(memory_engine_t *engine, const char *key);

int  hnsw_init(hnsw_graph_t *graph, uint32_t capacity);
void hnsw_destroy(hnsw_graph_t *graph);
int  hnsw_insert(hnsw_graph_t *graph, uint32_t node_id,
                 const int8_t *embedding, const mem_entry_t *entries);
int  hnsw_search(hnsw_graph_t *graph, const int8_t *query, uint32_t top_k,
                 uint32_t *result_ids, float *result_scores,
                 const mem_entry_t *entries);

float cosine_similarity_int8(const int8_t *a, const int8_t *b, uint32_t dim);

ssize_t memory_serialize(memory_engine_t *engine, void *buf, size_t size);
int     memory_deserialize(memory_engine_t *engine, const void *buf, size_t size);
