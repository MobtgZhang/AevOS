#include "memory.h"
/*
 * HNSW graph nodes and mem_entry_t rows use kmalloc/kcalloc (kernel slab);
 * optional mmap/SQLite cold tier is planned for L3 HMS cache.
 */
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "kernel/drivers/timer.h"
#include "lib/string.h"

/* ── Simple xorshift64 PRNG ─────────────────────────────── */

static uint64_t mem_rng_state = 0xA5A5A5A5DEADBEEFULL;

static uint64_t mem_rng_next(void) {
    mem_rng_state ^= mem_rng_state << 13;
    mem_rng_state ^= mem_rng_state >> 7;
    mem_rng_state ^= mem_rng_state << 17;
    return mem_rng_state;
}

static float mem_rng_uniform(void) {
    return (float)(mem_rng_next() & 0xFFFFFFu) / (float)0xFFFFFFu;
}

/* ── Fast math helpers ──────────────────────────────────── */

static float mem_expf(float x) {
    if (x > 88.72f)  return 3.4028235e+38f;
    if (x < -87.33f) return 0.0f;
    float kf = x * 1.4426950408889634f;
    int32_t k = (int32_t)(kf + (kf >= 0 ? 0.5f : -0.5f));
    float r = x - (float)k * 0.6931471805599453f;
    float r2 = r * r;
    float p = 1.0f + r + r2 * 0.5f + r2 * r * (1.0f / 6.0f)
            + r2 * r2 * (1.0f / 24.0f);
    union { float f; int32_t i; } u = { p };
    u.i += k << 23;
    return u.f;
}

static float mem_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } u = { x };
    u.i = (u.i >> 1) + 0x1FC00000u;
    u.f = 0.5f * (u.f + x / u.f);
    u.f = 0.5f * (u.f + x / u.f);
    return u.f;
}

static float mem_logf(float x) {
    if (x <= 0.0f) return -88.0f;
    union { float f; uint32_t i; } u = { x };
    float log2 = (float)((int32_t)u.i - (int32_t)0x3F800000) / (float)(1 << 23);
    return log2 * 0.6931471805599453f;
}

/* ── Cosine similarity for int8 quantized embeddings ──────── */

float cosine_similarity_int8(const int8_t *a, const int8_t *b, uint32_t dim) {
    int64_t dot = 0, norm_a = 0, norm_b = 0;
    for (uint32_t i = 0; i < dim; i++) {
        dot    += (int64_t)a[i] * b[i];
        norm_a += (int64_t)a[i] * a[i];
        norm_b += (int64_t)b[i] * b[i];
    }
    float denom = mem_sqrtf((float)norm_a) * mem_sqrtf((float)norm_b);
    if (denom < 1e-8f) return 0.0f;
    return (float)dot / denom;
}

/* ── HNSW implementation ─────────────────────────────────── */

int hnsw_init(hnsw_graph_t *graph, uint32_t capacity) {
    if (!graph) return -EINVAL;
    memset(graph, 0, sizeof(*graph));
    graph->nodes = (hnsw_node_t *)kcalloc(capacity, sizeof(hnsw_node_t));
    if (!graph->nodes) return -ENOMEM;
    graph->capacity = capacity;
    graph->max_level = 0;
    graph->entry_point = 0;
    graph->level_mult = 1.0f / mem_logf((float)HNSW_M);
    return 0;
}

void hnsw_destroy(hnsw_graph_t *graph) {
    if (!graph) return;
    kfree(graph->nodes);
    memset(graph, 0, sizeof(*graph));
}

static int hnsw_random_level(hnsw_graph_t *graph) {
    float r = mem_rng_uniform();
    if (r <= 0.0f) r = 1e-10f;
    int level = (int)(-mem_logf(r) * graph->level_mult);
    if (level >= HNSW_MAX_LEVEL) level = HNSW_MAX_LEVEL - 1;
    return level;
}

static float hnsw_distance(const int8_t *a, const int8_t *b) {
    return 1.0f - cosine_similarity_int8(a, b, EMBED_DIM);
}

/* Simple min-heap for nearest-neighbor search */
typedef struct {
    uint32_t id;
    float    dist;
} nn_pair_t;

static void heap_swap(nn_pair_t *a, nn_pair_t *b) {
    nn_pair_t t = *a; *a = *b; *b = t;
}

static void heap_push_max(nn_pair_t *heap, uint32_t *size, uint32_t cap,
                          uint32_t id, float dist) {
    if (*size < cap) {
        heap[*size] = (nn_pair_t){ id, dist };
        (*size)++;
        /* Sift up (max-heap) */
        uint32_t i = *size - 1;
        while (i > 0) {
            uint32_t parent = (i - 1) / 2;
            if (heap[parent].dist < heap[i].dist)
                { heap_swap(&heap[parent], &heap[i]); i = parent; }
            else break;
        }
    } else if (dist < heap[0].dist) {
        heap[0] = (nn_pair_t){ id, dist };
        /* Sift down */
        uint32_t i = 0;
        while (1) {
            uint32_t l = 2 * i + 1, r = 2 * i + 2, max_i = i;
            if (l < *size && heap[l].dist > heap[max_i].dist) max_i = l;
            if (r < *size && heap[r].dist > heap[max_i].dist) max_i = r;
            if (max_i == i) break;
            heap_swap(&heap[i], &heap[max_i]);
            i = max_i;
        }
    }
}

/* Greedy search at a single level, returning ef nearest neighbors */
static uint32_t hnsw_search_level(hnsw_graph_t *graph, const int8_t *query,
                                  uint32_t entry, int level, uint32_t ef,
                                  nn_pair_t *result, const mem_entry_t *entries) {
    bool *visited = (bool *)kcalloc(graph->count, sizeof(bool));
    if (!visited) return 0;

    nn_pair_t *candidates = (nn_pair_t *)kmalloc(ef * 4 * sizeof(nn_pair_t));
    if (!candidates) { kfree(visited); return 0; }

    float entry_dist = hnsw_distance(query, entries[entry].embedding);
    candidates[0] = (nn_pair_t){ entry, entry_dist };
    uint32_t cand_count = 1;
    uint32_t res_count = 0;

    heap_push_max(result, &res_count, ef, entry, entry_dist);
    visited[entry] = true;

    uint32_t cand_idx = 0;
    while (cand_idx < cand_count) {
        /* Pick the closest unprocessed candidate */
        uint32_t best_ci = cand_idx;
        for (uint32_t ci = cand_idx + 1; ci < cand_count; ci++)
            if (candidates[ci].dist < candidates[best_ci].dist) best_ci = ci;
        if (best_ci != cand_idx)
            heap_swap(&candidates[cand_idx], &candidates[best_ci]);

        nn_pair_t cur = candidates[cand_idx++];

        /* If current candidate is further than worst in result, stop */
        if (res_count >= ef && cur.dist > result[0].dist)
            break;

        /* Explore neighbors */
        hnsw_node_t *node = &graph->nodes[cur.id];
        uint32_t n_neigh = node->n_neighbors[level];
        uint32_t max_n = (level == 0) ? HNSW_M0 : HNSW_M;
        if (n_neigh > max_n) n_neigh = max_n;

        for (uint32_t ni = 0; ni < n_neigh; ni++) {
            uint32_t nid = node->neighbors[level][ni];
            if (nid >= graph->count || visited[nid]) continue;
            visited[nid] = true;

            float d = hnsw_distance(query, entries[nid].embedding);
            if (res_count < ef || d < result[0].dist) {
                heap_push_max(result, &res_count, ef, nid, d);
                if (cand_count < ef * 4)
                    candidates[cand_count++] = (nn_pair_t){ nid, d };
            }
        }
    }

    kfree(candidates);
    kfree(visited);
    return res_count;
}

/* Add neighbor to a node, keeping only the best M connections */
static void hnsw_add_neighbor(hnsw_graph_t *graph, uint32_t node_id,
                              uint32_t neighbor_id, int level,
                              const mem_entry_t *entries) {
    hnsw_node_t *node = &graph->nodes[node_id];
    uint32_t max_conn = (level == 0) ? HNSW_M0 : HNSW_M;
    uint32_t n = node->n_neighbors[level];

    /* Check for duplicate */
    for (uint32_t i = 0; i < n; i++)
        if (node->neighbors[level][i] == neighbor_id) return;

    if (n < max_conn) {
        node->neighbors[level][n] = neighbor_id;
        node->n_neighbors[level] = n + 1;
    } else {
        /* Replace the most distant neighbor if new one is closer */
        float new_dist = hnsw_distance(entries[node_id].embedding,
                                       entries[neighbor_id].embedding);
        uint32_t worst_i = 0;
        float worst_d = hnsw_distance(entries[node_id].embedding,
                                      entries[node->neighbors[level][0]].embedding);
        for (uint32_t i = 1; i < n; i++) {
            float d = hnsw_distance(entries[node_id].embedding,
                                    entries[node->neighbors[level][i]].embedding);
            if (d > worst_d) { worst_d = d; worst_i = i; }
        }
        if (new_dist < worst_d)
            node->neighbors[level][worst_i] = neighbor_id;
    }
}

int hnsw_insert(hnsw_graph_t *graph, uint32_t node_id,
                const int8_t *embedding, const mem_entry_t *entries) {
    if (!graph || node_id >= graph->capacity) return -EINVAL;

    int node_level = hnsw_random_level(graph);
    hnsw_node_t *new_node = &graph->nodes[node_id];
    new_node->id = node_id;
    new_node->level = (uint8_t)node_level;
    memset(new_node->n_neighbors, 0, sizeof(new_node->n_neighbors));

    if (graph->count == 0) {
        graph->entry_point = node_id;
        graph->max_level = node_level;
        graph->count = 1;
        return 0;
    }

    uint32_t cur_entry = graph->entry_point;

    /* Greedily descend from top level to node_level + 1 */
    for (int l = graph->max_level; l > node_level; l--) {
        bool improved = true;
        while (improved) {
            improved = false;
            hnsw_node_t *cur_node = &graph->nodes[cur_entry];
            float cur_dist = hnsw_distance(embedding, entries[cur_entry].embedding);
            uint32_t nn = cur_node->n_neighbors[l];
            for (uint32_t ni = 0; ni < nn; ni++) {
                uint32_t nid = cur_node->neighbors[l][ni];
                if (nid >= graph->count) continue;
                float d = hnsw_distance(embedding, entries[nid].embedding);
                if (d < cur_dist) {
                    cur_entry = nid;
                    cur_dist = d;
                    improved = true;
                }
            }
        }
    }

    /* For levels node_level..0, search and connect */
    uint32_t ef = HNSW_EF_CONSTRUCT;
    nn_pair_t *neighbors = (nn_pair_t *)kmalloc(ef * sizeof(nn_pair_t));
    if (!neighbors) return -ENOMEM;

    for (int l = MIN(node_level, graph->max_level); l >= 0; l--) {
        uint32_t found = hnsw_search_level(graph, embedding, cur_entry, l,
                                           ef, neighbors, entries);
        uint32_t max_conn = (l == 0) ? HNSW_M0 : HNSW_M;
        uint32_t to_connect = MIN(found, max_conn);

        /* Sort neighbors by distance (simple selection sort, small N) */
        for (uint32_t i = 0; i < to_connect; i++) {
            uint32_t min_j = i;
            for (uint32_t j = i + 1; j < found; j++)
                if (neighbors[j].dist < neighbors[min_j].dist) min_j = j;
            if (min_j != i) heap_swap(&neighbors[i], &neighbors[min_j]);
        }

        for (uint32_t i = 0; i < to_connect; i++) {
            hnsw_add_neighbor(graph, node_id, neighbors[i].id, l, entries);
            hnsw_add_neighbor(graph, neighbors[i].id, node_id, l, entries);
        }

        if (found > 0) cur_entry = neighbors[0].id;
    }

    kfree(neighbors);

    if (node_level > graph->max_level) {
        graph->max_level = node_level;
        graph->entry_point = node_id;
    }

    if (node_id >= graph->count) graph->count = node_id + 1;
    return 0;
}

int hnsw_search(hnsw_graph_t *graph, const int8_t *query, uint32_t top_k,
                uint32_t *result_ids, float *result_scores,
                const mem_entry_t *entries) {
    if (!graph || !query || graph->count == 0) return 0;

    uint32_t cur_entry = graph->entry_point;

    /* Greedy descent from top to level 1 */
    for (int l = graph->max_level; l > 0; l--) {
        bool improved = true;
        while (improved) {
            improved = false;
            hnsw_node_t *cur_node = &graph->nodes[cur_entry];
            float cur_dist = hnsw_distance(query, entries[cur_entry].embedding);
            uint32_t nn = cur_node->n_neighbors[l];
            for (uint32_t ni = 0; ni < nn; ni++) {
                uint32_t nid = cur_node->neighbors[l][ni];
                if (nid >= graph->count) continue;
                float d = hnsw_distance(query, entries[nid].embedding);
                if (d < cur_dist) {
                    cur_entry = nid;
                    cur_dist = d;
                    improved = true;
                }
            }
        }
    }

    /* Search at level 0 with ef_search width */
    uint32_t ef = MAX(top_k, HNSW_EF_SEARCH);
    nn_pair_t *results = (nn_pair_t *)kmalloc(ef * sizeof(nn_pair_t));
    if (!results) return 0;

    uint32_t found = hnsw_search_level(graph, query, cur_entry, 0, ef,
                                       results, entries);

    /* Sort by distance and return top_k */
    for (uint32_t i = 0; i < found; i++) {
        uint32_t min_j = i;
        for (uint32_t j = i + 1; j < found; j++)
            if (results[j].dist < results[min_j].dist) min_j = j;
        if (min_j != i) heap_swap(&results[i], &results[min_j]);
    }

    uint32_t n = MIN(found, top_k);
    for (uint32_t i = 0; i < n; i++) {
        if (result_ids)    result_ids[i] = results[i].id;
        if (result_scores) result_scores[i] = 1.0f - results[i].dist;
    }

    kfree(results);
    return (int)n;
}

/* ── Memory Engine ───────────────────────────────────────── */

#define MEM_INITIAL_CAP 1024

int memory_init(memory_engine_t *engine) {
    if (!engine) return -EINVAL;
    memset(engine, 0, sizeof(*engine));

    engine->capacity = MEM_INITIAL_CAP;
    engine->entries = (mem_entry_t *)kcalloc(engine->capacity, sizeof(mem_entry_t));
    if (!engine->entries) return -ENOMEM;

    engine->index = (hnsw_graph_t *)kmalloc(sizeof(hnsw_graph_t));
    if (!engine->index) { kfree(engine->entries); return -ENOMEM; }
    int rc = hnsw_init(engine->index, engine->capacity);
    if (rc < 0) {
        kfree(engine->index);
        kfree(engine->entries);
        return rc;
    }

    engine->working_mem = hashmap_create(64);
    if (!engine->working_mem) {
        hnsw_destroy(engine->index);
        kfree(engine->index);
        kfree(engine->entries);
        return -ENOMEM;
    }

    engine->next_id = 1;
    engine->lock = SPINLOCK_INIT;
    return 0;
}

void memory_destroy(memory_engine_t *engine) {
    if (!engine) return;
    for (uint32_t i = 0; i < engine->count; i++)
        kfree(engine->entries[i].content);
    kfree(engine->entries);
    if (engine->index) {
        hnsw_destroy(engine->index);
        kfree(engine->index);
    }
    if (engine->working_mem)
        hashmap_destroy(engine->working_mem);
    memset(engine, 0, sizeof(*engine));
}

static int memory_grow(memory_engine_t *engine) {
    uint32_t new_cap = engine->capacity * 2;
    if (new_cap > MEM_MAX_ENTRIES) new_cap = MEM_MAX_ENTRIES;
    if (new_cap <= engine->capacity) return -ENOSPC;

    mem_entry_t *new_entries = (mem_entry_t *)krealloc(engine->entries,
                                                       new_cap * sizeof(mem_entry_t));
    if (!new_entries) return -ENOMEM;
    memset(new_entries + engine->capacity, 0,
           (new_cap - engine->capacity) * sizeof(mem_entry_t));
    engine->entries = new_entries;

    /* Rebuild HNSW graph with larger capacity */
    hnsw_graph_t *new_graph = (hnsw_graph_t *)kmalloc(sizeof(hnsw_graph_t));
    if (!new_graph) return -ENOMEM;
    int rc = hnsw_init(new_graph, new_cap);
    if (rc < 0) { kfree(new_graph); return rc; }

    for (uint32_t i = 0; i < engine->count; i++)
        hnsw_insert(new_graph, i, engine->entries[i].embedding, engine->entries);

    hnsw_destroy(engine->index);
    kfree(engine->index);
    engine->index = new_graph;
    engine->capacity = new_cap;
    return 0;
}

uint64_t memory_store(memory_engine_t *engine, const void *content,
                      size_t content_len, const int8_t *embedding,
                      float importance) {
    if (!engine || !content || !embedding) return 0;

    spin_lock(&engine->lock);

    if (engine->count >= engine->capacity) {
        if (memory_grow(engine) < 0) {
            spin_unlock(&engine->lock);
            return 0;
        }
    }

    uint32_t idx = engine->count;
    mem_entry_t *e = &engine->entries[idx];

    e->id = engine->next_id++;
    memcpy(e->embedding, embedding, EMBED_DIM);
    e->importance = importance;
    e->last_access = timer_get_ms();
    e->access_count = 1;
    e->content_len = (uint32_t)content_len;
    e->content = (uint8_t *)kmalloc(content_len + 1);
    if (e->content) {
        memcpy(e->content, content, content_len);
        e->content[content_len] = '\0';
    }

    hnsw_insert(engine->index, idx, embedding, engine->entries);
    engine->count++;

    uint64_t id = e->id;
    spin_unlock(&engine->lock);
    return id;
}

int memory_retrieve(memory_engine_t *engine, const int8_t *query_embedding,
                    uint32_t top_k, mem_result_t *results) {
    if (!engine || !query_embedding || !results) return -EINVAL;
    if (engine->count == 0) return 0;

    spin_lock(&engine->lock);

    uint32_t *ids = (uint32_t *)kmalloc(top_k * sizeof(uint32_t));
    float *scores = (float *)kmalloc(top_k * sizeof(float));
    if (!ids || !scores) {
        kfree(ids); kfree(scores);
        spin_unlock(&engine->lock);
        return -ENOMEM;
    }

    int found = hnsw_search(engine->index, query_embedding, top_k,
                            ids, scores, engine->entries);

    for (int i = 0; i < found; i++) {
        uint32_t idx = ids[i];
        results[i].id = engine->entries[idx].id;
        results[i].score = scores[i];
        engine->entries[idx].last_access = timer_get_ms();
        engine->entries[idx].access_count++;
    }

    kfree(ids);
    kfree(scores);
    spin_unlock(&engine->lock);
    return found;
}

void memory_update_importance(memory_engine_t *engine, uint64_t id,
                              float new_importance) {
    if (!engine) return;
    spin_lock(&engine->lock);
    for (uint32_t i = 0; i < engine->count; i++) {
        if (engine->entries[i].id == id) {
            engine->entries[i].importance = new_importance;
            break;
        }
    }
    spin_unlock(&engine->lock);
}

/* ── Ebbinghaus forgetting curve ─────────────────────────── */
/* R = e^(-t/S)  where S = stability ∝ access_count * importance */

void memory_forget(memory_engine_t *engine) {
    if (!engine || engine->count == 0) return;

    spin_lock(&engine->lock);
    uint64_t now = timer_get_ms();

    for (uint32_t i = 0; i < engine->count; i++) {
        mem_entry_t *e = &engine->entries[i];
        float stability = (float)e->access_count * e->importance;
        if (stability < 1.0f) stability = 1.0f;

        float elapsed_hours = (float)(now - e->last_access) / 3600000.0f;
        float retention = mem_expf(-elapsed_hours / stability);

        /* Decay importance; if retention drops below threshold, mark for removal */
        e->importance *= retention;

        if (e->importance < 0.01f && e->access_count <= 1) {
            kfree(e->content);
            e->content = NULL;
            e->content_len = 0;

            /* Swap with last entry to compact the array */
            if (i < engine->count - 1) {
                engine->entries[i] = engine->entries[engine->count - 1];
                memset(&engine->entries[engine->count - 1], 0, sizeof(mem_entry_t));
            }
            engine->count--;
            i--;
        }
    }

    spin_unlock(&engine->lock);
}

/* ── Working memory (hot key-value store) ────────────────── */

void memory_working_set(memory_engine_t *engine, const char *key, void *value) {
    if (!engine || !key || !engine->working_mem) return;
    hashmap_put(engine->working_mem, key, value);
}

void *memory_working_get(memory_engine_t *engine, const char *key) {
    if (!engine || !key || !engine->working_mem) return NULL;
    return hashmap_get(engine->working_mem, key);
}

/* ── Serialization ───────────────────────────────────────── */

#define MEM_SERIAL_MAGIC 0x4D454D53u /* "MEMS" */

ssize_t memory_serialize(memory_engine_t *engine, void *buf, size_t size) {
    if (!engine || !buf) return -EINVAL;

    spin_lock(&engine->lock);

    /* Calculate needed size */
    size_t needed = 12; /* magic + count + next_id (4+4+8=16, round) */
    needed = 16;
    for (uint32_t i = 0; i < engine->count; i++) {
        mem_entry_t *e = &engine->entries[i];
        needed += 8 + EMBED_DIM + 4 + 8 + 4 + 4 + e->content_len;
    }

    if (size < needed) { spin_unlock(&engine->lock); return (ssize_t)needed; }

    uint8_t *p = (uint8_t *)buf;
    uint32_t magic = MEM_SERIAL_MAGIC;
    memcpy(p, &magic, 4); p += 4;
    memcpy(p, &engine->count, 4); p += 4;
    memcpy(p, &engine->next_id, 8); p += 8;

    for (uint32_t i = 0; i < engine->count; i++) {
        mem_entry_t *e = &engine->entries[i];
        memcpy(p, &e->id, 8);        p += 8;
        memcpy(p, e->embedding, EMBED_DIM); p += EMBED_DIM;
        memcpy(p, &e->importance, 4); p += 4;
        memcpy(p, &e->last_access, 8); p += 8;
        memcpy(p, &e->access_count, 4); p += 4;
        memcpy(p, &e->content_len, 4); p += 4;
        if (e->content && e->content_len > 0) {
            memcpy(p, e->content, e->content_len);
            p += e->content_len;
        }
    }

    spin_unlock(&engine->lock);
    return (ssize_t)(p - (uint8_t *)buf);
}

int memory_deserialize(memory_engine_t *engine, const void *buf, size_t size) {
    if (!engine || !buf || size < 16) return -EINVAL;

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t magic;
    memcpy(&magic, p, 4); p += 4;
    if (magic != MEM_SERIAL_MAGIC) return -EINVAL;

    uint32_t count;
    uint64_t next_id;
    memcpy(&count, p, 4);    p += 4;
    memcpy(&next_id, p, 8);  p += 8;

    /* Ensure enough capacity */
    while (engine->capacity < count) {
        if (memory_grow(engine) < 0) return -ENOMEM;
    }

    engine->count = 0;
    engine->next_id = next_id;

    for (uint32_t i = 0; i < count; i++) {
        if ((size_t)(p - (const uint8_t *)buf) + 8 + EMBED_DIM + 20 > size) break;

        mem_entry_t *e = &engine->entries[i];
        memcpy(&e->id, p, 8);              p += 8;
        memcpy(e->embedding, p, EMBED_DIM); p += EMBED_DIM;
        memcpy(&e->importance, p, 4);       p += 4;
        memcpy(&e->last_access, p, 8);      p += 8;
        memcpy(&e->access_count, p, 4);     p += 4;
        memcpy(&e->content_len, p, 4);      p += 4;

        if (e->content_len > 0 && (size_t)(p - (const uint8_t *)buf) + e->content_len <= size) {
            e->content = (uint8_t *)kmalloc(e->content_len + 1);
            if (e->content) {
                memcpy(e->content, p, e->content_len);
                e->content[e->content_len] = '\0';
            }
            p += e->content_len;
        }

        hnsw_insert(engine->index, i, e->embedding, engine->entries);
        engine->count++;
    }

    return 0;
}
