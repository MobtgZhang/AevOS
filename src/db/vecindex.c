#include "vecindex.h"
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "lib/string.h"

/*
 * AevOS Vector Index — FAISS-inspired quantized vector search.
 *
 * All arithmetic uses int8_t vectors with int32_t accumulators.
 * No floating-point operations required.
 *
 * Flat index: brute-force linear scan over all stored vectors.
 * IVF index:  inverted-file with K-means centroids; only nprobe
 *             closest partitions are searched (approximate NN).
 */

#define VEC_BLOCK_SIZE    256   /* vectors per allocation block */
#define IVF_DEFAULT_NLIST 64
#define IVF_DEFAULT_NPROBE 8
#define KMEANS_MAX_ITERS  20

/* ── Per-vector entry ────────────────────────────────── */

typedef struct {
    uint64_t id;
    int8_t  *vec;
} vec_entry_t;

/* ── Inverted list (one per centroid) ────────────────── */

typedef struct {
    vec_entry_t *entries;
    uint32_t     count;
    uint32_t     capacity;
} ivf_list_t;

/* ── Index structure ─────────────────────────────────── */

struct vecindex {
    vecindex_type_t type;
    vecindex_dist_t dist;
    uint32_t        dim;
    spinlock_t      lock;

    /* Flat index storage */
    vec_entry_t    *flat_entries;
    uint32_t        flat_count;
    uint32_t        flat_capacity;

    /* IVF index storage */
    int8_t         *centroids;    /* nlist * dim */
    ivf_list_t     *lists;
    uint32_t        nlist;
    uint32_t        nprobe;
    bool            trained;
};

/* ── Distance functions ──────────────────────────────── */

static int32_t dist_l2_sq(const int8_t *a, const int8_t *b, uint32_t dim)
{
    int32_t sum = 0;
    for (uint32_t i = 0; i < dim; i++) {
        int32_t d = (int32_t)a[i] - (int32_t)b[i];
        sum += d * d;
    }
    return sum;
}

static int32_t dist_ip(const int8_t *a, const int8_t *b, uint32_t dim)
{
    int32_t sum = 0;
    for (uint32_t i = 0; i < dim; i++)
        sum += (int32_t)a[i] * (int32_t)b[i];
    return sum;
}

/* ── Min-heap for top-k selection ────────────────────── */

typedef struct {
    uint64_t id;
    int32_t  dist;
} heap_item_t;

static void heap_sift_down(heap_item_t *h, uint32_t n, uint32_t i, bool max_heap)
{
    while (1) {
        uint32_t target = i;
        uint32_t l = 2 * i + 1, r = 2 * i + 2;
        if (max_heap) {
            if (l < n && h[l].dist > h[target].dist) target = l;
            if (r < n && h[r].dist > h[target].dist) target = r;
        } else {
            if (l < n && h[l].dist < h[target].dist) target = l;
            if (r < n && h[r].dist < h[target].dist) target = r;
        }
        if (target == i) break;
        heap_item_t tmp = h[i]; h[i] = h[target]; h[target] = tmp;
        i = target;
    }
}

/*
 * For L2 (lower = better): use a MAX-heap of size k.
 *   If new dist < heap root, replace root and sift down.
 * For IP (higher = better): use a MIN-heap of size k.
 *   If new dist > heap root, replace root and sift down.
 */

/* ── Flat index operations ───────────────────────────── */

static int flat_add(vecindex_t *idx, uint64_t id, const int8_t *vec)
{
    if (idx->flat_count >= idx->flat_capacity) {
        uint32_t new_cap = idx->flat_capacity + VEC_BLOCK_SIZE;
        vec_entry_t *new_arr = (vec_entry_t *)krealloc(
            idx->flat_entries, new_cap * sizeof(vec_entry_t));
        if (!new_arr) return -ENOMEM;
        idx->flat_entries = new_arr;
        idx->flat_capacity = new_cap;
    }

    int8_t *copy = (int8_t *)kmalloc(idx->dim);
    if (!copy) return -ENOMEM;
    memcpy(copy, vec, idx->dim);

    idx->flat_entries[idx->flat_count].id  = id;
    idx->flat_entries[idx->flat_count].vec = copy;
    idx->flat_count++;
    return 0;
}

static int flat_search(vecindex_t *idx, const int8_t *query, uint32_t k,
                       uint64_t *out_ids, int32_t *out_dists)
{
    if (k == 0 || idx->flat_count == 0) return 0;
    if (k > idx->flat_count) k = idx->flat_count;

    heap_item_t *heap = (heap_item_t *)kmalloc(k * sizeof(heap_item_t));
    if (!heap) return -ENOMEM;

    bool is_l2 = (idx->dist == VECDIST_L2);
    uint32_t heap_size = 0;

    for (uint32_t i = 0; i < idx->flat_count; i++) {
        int32_t d = is_l2
            ? dist_l2_sq(query, idx->flat_entries[i].vec, idx->dim)
            : dist_ip(query, idx->flat_entries[i].vec, idx->dim);

        if (heap_size < k) {
            heap[heap_size].id   = idx->flat_entries[i].id;
            heap[heap_size].dist = d;
            heap_size++;
            if (heap_size == k) {
                for (int j = (int)k / 2 - 1; j >= 0; j--)
                    heap_sift_down(heap, k, (uint32_t)j, is_l2);
            }
        } else {
            bool better = is_l2 ? (d < heap[0].dist) : (d > heap[0].dist);
            if (better) {
                heap[0].id   = idx->flat_entries[i].id;
                heap[0].dist = d;
                heap_sift_down(heap, k, 0, is_l2);
            }
        }
    }

    /* Extract sorted results */
    uint32_t n = heap_size;
    for (uint32_t i = n; i > 0; i--) {
        out_ids[i - 1]   = heap[0].id;
        out_dists[i - 1] = heap[0].dist;
        heap[0] = heap[i - 1];
        if (i > 1)
            heap_sift_down(heap, i - 1, 0, is_l2);
    }

    kfree(heap);
    return (int)n;
}

/* ── IVF operations ──────────────────────────────────── */

static uint32_t find_nearest_centroid(vecindex_t *idx, const int8_t *vec)
{
    uint32_t best = 0;
    int32_t best_dist = dist_l2_sq(vec, idx->centroids, idx->dim);

    for (uint32_t c = 1; c < idx->nlist; c++) {
        int32_t d = dist_l2_sq(vec, idx->centroids + c * idx->dim, idx->dim);
        if (d < best_dist) {
            best_dist = d;
            best = c;
        }
    }
    return best;
}

static int ivf_add(vecindex_t *idx, uint64_t id, const int8_t *vec)
{
    if (!idx->trained) return -EINVAL;

    uint32_t c = find_nearest_centroid(idx, vec);
    ivf_list_t *list = &idx->lists[c];

    if (list->count >= list->capacity) {
        uint32_t new_cap = list->capacity + VEC_BLOCK_SIZE;
        vec_entry_t *new_arr = (vec_entry_t *)krealloc(
            list->entries, new_cap * sizeof(vec_entry_t));
        if (!new_arr) return -ENOMEM;
        list->entries = new_arr;
        list->capacity = new_cap;
    }

    int8_t *copy = (int8_t *)kmalloc(idx->dim);
    if (!copy) return -ENOMEM;
    memcpy(copy, vec, idx->dim);

    list->entries[list->count].id  = id;
    list->entries[list->count].vec = copy;
    list->count++;
    return 0;
}

static int ivf_search(vecindex_t *idx, const int8_t *query, uint32_t k,
                      uint64_t *out_ids, int32_t *out_dists)
{
    if (!idx->trained || k == 0) return 0;

    /* Find nprobe nearest centroids */
    uint32_t nprobe = MIN(idx->nprobe, idx->nlist);
    uint32_t *probe = (uint32_t *)kmalloc(nprobe * sizeof(uint32_t));
    int32_t  *cdist = (int32_t *)kmalloc(idx->nlist * sizeof(int32_t));
    if (!probe || !cdist) {
        if (probe) kfree(probe);
        if (cdist) kfree(cdist);
        return -ENOMEM;
    }

    for (uint32_t c = 0; c < idx->nlist; c++)
        cdist[c] = dist_l2_sq(query, idx->centroids + c * idx->dim, idx->dim);

    for (uint32_t p = 0; p < nprobe; p++) {
        uint32_t best = 0;
        for (uint32_t c = 1; c < idx->nlist; c++) {
            if (cdist[c] < cdist[best]) best = c;
        }
        probe[p] = best;
        cdist[best] = INT32_MAX;
    }
    kfree(cdist);

    heap_item_t *heap = (heap_item_t *)kmalloc(k * sizeof(heap_item_t));
    if (!heap) { kfree(probe); return -ENOMEM; }

    bool is_l2 = (idx->dist == VECDIST_L2);
    uint32_t heap_size = 0;

    for (uint32_t p = 0; p < nprobe; p++) {
        ivf_list_t *list = &idx->lists[probe[p]];
        for (uint32_t i = 0; i < list->count; i++) {
            int32_t d = is_l2
                ? dist_l2_sq(query, list->entries[i].vec, idx->dim)
                : dist_ip(query, list->entries[i].vec, idx->dim);

            if (heap_size < k) {
                heap[heap_size].id   = list->entries[i].id;
                heap[heap_size].dist = d;
                heap_size++;
                if (heap_size == k) {
                    for (int j = (int)k / 2 - 1; j >= 0; j--)
                        heap_sift_down(heap, k, (uint32_t)j, is_l2);
                }
            } else {
                bool better = is_l2 ? (d < heap[0].dist) : (d > heap[0].dist);
                if (better) {
                    heap[0].id   = list->entries[i].id;
                    heap[0].dist = d;
                    heap_sift_down(heap, k, 0, is_l2);
                }
            }
        }
    }
    kfree(probe);

    uint32_t n = heap_size;
    for (uint32_t i = n; i > 0; i--) {
        out_ids[i - 1]   = heap[0].id;
        out_dists[i - 1] = heap[0].dist;
        heap[0] = heap[i - 1];
        if (i > 1)
            heap_sift_down(heap, i - 1, 0, is_l2);
    }

    kfree(heap);
    return (int)n;
}

/* ── K-means training (Lloyd's algorithm with int8 vectors) ── */

int vecindex_train(vecindex_t *idx, const int8_t *vecs, uint32_t count)
{
    if (!idx || !vecs || count == 0) return -EINVAL;
    if (idx->type != VECINDEX_IVF) return -EINVAL;

    uint32_t nlist = idx->nlist;
    if (nlist > count) nlist = count;

    if (!idx->centroids) {
        idx->centroids = (int8_t *)kmalloc(nlist * idx->dim);
        if (!idx->centroids) return -ENOMEM;
    }

    /* Initialize centroids from first nlist vectors */
    for (uint32_t c = 0; c < nlist; c++)
        memcpy(idx->centroids + c * idx->dim, vecs + c * idx->dim, idx->dim);

    int32_t *accum = (int32_t *)kmalloc(nlist * idx->dim * sizeof(int32_t));
    uint32_t *counts = (uint32_t *)kcalloc(nlist, sizeof(uint32_t));
    if (!accum || !counts) {
        if (accum) kfree(accum);
        if (counts) kfree(counts);
        return -ENOMEM;
    }

    for (uint32_t iter = 0; iter < KMEANS_MAX_ITERS; iter++) {
        memset(accum, 0, nlist * idx->dim * sizeof(int32_t));
        memset(counts, 0, nlist * sizeof(uint32_t));

        /* Assign each vector to the nearest centroid */
        for (uint32_t i = 0; i < count; i++) {
            const int8_t *v = vecs + i * idx->dim;
            uint32_t best = 0;
            int32_t best_d = dist_l2_sq(v, idx->centroids, idx->dim);

            for (uint32_t c = 1; c < nlist; c++) {
                int32_t d = dist_l2_sq(v, idx->centroids + c * idx->dim, idx->dim);
                if (d < best_d) { best_d = d; best = c; }
            }

            counts[best]++;
            int32_t *a = accum + best * idx->dim;
            for (uint32_t d = 0; d < idx->dim; d++)
                a[d] += v[d];
        }

        /* Recompute centroids */
        for (uint32_t c = 0; c < nlist; c++) {
            if (counts[c] == 0) continue;
            int8_t *cent = idx->centroids + c * idx->dim;
            int32_t *a = accum + c * idx->dim;
            for (uint32_t d = 0; d < idx->dim; d++) {
                int32_t avg = a[d] / (int32_t)counts[c];
                if (avg > 127) avg = 127;
                if (avg < -128) avg = -128;
                cent[d] = (int8_t)avg;
            }
        }
    }

    kfree(accum);
    kfree(counts);

    idx->trained = true;
    klog("[vecindex] IVF trained: %u centroids, %u training vectors\n", nlist, count);
    return 0;
}

/* ── Public API ──────────────────────────────────────── */

vecindex_t *vecindex_create(uint32_t dim, vecindex_type_t type,
                            vecindex_dist_t dist)
{
    vecindex_t *idx = (vecindex_t *)kcalloc(1, sizeof(vecindex_t));
    if (!idx) return NULL;

    idx->type = type;
    idx->dist = dist;
    idx->dim  = dim;
    idx->lock = SPINLOCK_INIT;

    if (type == VECINDEX_IVF) {
        idx->nlist  = IVF_DEFAULT_NLIST;
        idx->nprobe = IVF_DEFAULT_NPROBE;
        idx->lists  = (ivf_list_t *)kcalloc(idx->nlist, sizeof(ivf_list_t));
        if (!idx->lists) { kfree(idx); return NULL; }
    }

    klog("[vecindex] created %s index dim=%u dist=%s\n",
         type == VECINDEX_FLAT ? "flat" : "IVF",
         dim, dist == VECDIST_L2 ? "L2" : "IP");
    return idx;
}

void vecindex_destroy(vecindex_t *idx)
{
    if (!idx) return;

    for (uint32_t i = 0; i < idx->flat_count; i++) {
        if (idx->flat_entries[i].vec) kfree(idx->flat_entries[i].vec);
    }
    if (idx->flat_entries) kfree(idx->flat_entries);

    if (idx->lists) {
        for (uint32_t c = 0; c < idx->nlist; c++) {
            for (uint32_t i = 0; i < idx->lists[c].count; i++) {
                if (idx->lists[c].entries[i].vec)
                    kfree(idx->lists[c].entries[i].vec);
            }
            if (idx->lists[c].entries) kfree(idx->lists[c].entries);
        }
        kfree(idx->lists);
    }

    if (idx->centroids) kfree(idx->centroids);
    kfree(idx);
}

int vecindex_add(vecindex_t *idx, uint64_t id, const int8_t *vec)
{
    if (!idx || !vec) return -EINVAL;
    spin_lock(&idx->lock);

    int ret;
    if (idx->type == VECINDEX_FLAT)
        ret = flat_add(idx, id, vec);
    else
        ret = ivf_add(idx, id, vec);

    spin_unlock(&idx->lock);
    return ret;
}

int vecindex_add_batch(vecindex_t *idx, const uint64_t *ids,
                       const int8_t *vecs, uint32_t count)
{
    if (!idx || !ids || !vecs) return -EINVAL;
    spin_lock(&idx->lock);

    int ret = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (idx->type == VECINDEX_FLAT)
            ret = flat_add(idx, ids[i], vecs + i * idx->dim);
        else
            ret = ivf_add(idx, ids[i], vecs + i * idx->dim);
        if (ret != 0) break;
    }

    spin_unlock(&idx->lock);
    return ret;
}

int vecindex_search(vecindex_t *idx, const int8_t *query, uint32_t k,
                    uint64_t *out_ids, int32_t *out_dists)
{
    if (!idx || !query || !out_ids || !out_dists || k == 0) return -EINVAL;
    spin_lock(&idx->lock);

    int ret;
    if (idx->type == VECINDEX_FLAT)
        ret = flat_search(idx, query, k, out_ids, out_dists);
    else
        ret = ivf_search(idx, query, k, out_ids, out_dists);

    spin_unlock(&idx->lock);
    return ret;
}

int vecindex_remove(vecindex_t *idx, uint64_t id)
{
    if (!idx) return -EINVAL;
    spin_lock(&idx->lock);

    if (idx->type == VECINDEX_FLAT) {
        for (uint32_t i = 0; i < idx->flat_count; i++) {
            if (idx->flat_entries[i].id == id) {
                if (idx->flat_entries[i].vec)
                    kfree(idx->flat_entries[i].vec);
                for (uint32_t j = i; j < idx->flat_count - 1; j++)
                    idx->flat_entries[j] = idx->flat_entries[j + 1];
                idx->flat_count--;
                spin_unlock(&idx->lock);
                return 0;
            }
        }
    } else {
        for (uint32_t c = 0; c < idx->nlist; c++) {
            ivf_list_t *list = &idx->lists[c];
            for (uint32_t i = 0; i < list->count; i++) {
                if (list->entries[i].id == id) {
                    if (list->entries[i].vec)
                        kfree(list->entries[i].vec);
                    for (uint32_t j = i; j < list->count - 1; j++)
                        list->entries[j] = list->entries[j + 1];
                    list->count--;
                    spin_unlock(&idx->lock);
                    return 0;
                }
            }
        }
    }

    spin_unlock(&idx->lock);
    return -ENOENT;
}

uint32_t vecindex_count(vecindex_t *idx)
{
    if (!idx) return 0;

    if (idx->type == VECINDEX_FLAT) return idx->flat_count;

    uint32_t total = 0;
    for (uint32_t c = 0; c < idx->nlist; c++)
        total += idx->lists[c].count;
    return total;
}

/* ── Serialization ───────────────────────────────────── */

#define VECINDEX_MAGIC 0x56454349u /* "VECI" */

ssize_t vecindex_serialize(vecindex_t *idx, void *buf, size_t size)
{
    if (!idx || !buf) return -EINVAL;
    spin_lock(&idx->lock);

    uint32_t count = vecindex_count(idx);
    size_t needed = 24 + (size_t)count * (8 + idx->dim);

    if (idx->type == VECINDEX_IVF && idx->centroids)
        needed += (size_t)idx->nlist * idx->dim;

    if (size < needed) { spin_unlock(&idx->lock); return (ssize_t)needed; }

    uint8_t *p = (uint8_t *)buf;
    uint32_t magic = VECINDEX_MAGIC;
    memcpy(p, &magic, 4);          p += 4;
    memcpy(p, &idx->type, 4);      p += 4;
    memcpy(p, &idx->dist, 4);      p += 4;
    memcpy(p, &idx->dim, 4);       p += 4;
    memcpy(p, &count, 4);          p += 4;
    memcpy(p, &idx->nlist, 4);     p += 4;

    /* Centroids (IVF) */
    if (idx->type == VECINDEX_IVF && idx->centroids) {
        memcpy(p, idx->centroids, idx->nlist * idx->dim);
        p += idx->nlist * idx->dim;
    }

    /* Vectors */
    if (idx->type == VECINDEX_FLAT) {
        for (uint32_t i = 0; i < idx->flat_count; i++) {
            memcpy(p, &idx->flat_entries[i].id, 8); p += 8;
            memcpy(p, idx->flat_entries[i].vec, idx->dim); p += idx->dim;
        }
    } else {
        for (uint32_t c = 0; c < idx->nlist; c++) {
            ivf_list_t *list = &idx->lists[c];
            for (uint32_t i = 0; i < list->count; i++) {
                memcpy(p, &list->entries[i].id, 8); p += 8;
                memcpy(p, list->entries[i].vec, idx->dim); p += idx->dim;
            }
        }
    }

    spin_unlock(&idx->lock);
    return (ssize_t)(p - (uint8_t *)buf);
}

vecindex_t *vecindex_deserialize(const void *buf, size_t size)
{
    if (!buf || size < 24) return NULL;

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t magic, type, dist, dim, count, nlist;
    memcpy(&magic, p, 4); p += 4;
    if (magic != VECINDEX_MAGIC) return NULL;

    memcpy(&type, p, 4);  p += 4;
    memcpy(&dist, p, 4);  p += 4;
    memcpy(&dim, p, 4);   p += 4;
    memcpy(&count, p, 4); p += 4;
    memcpy(&nlist, p, 4); p += 4;

    vecindex_t *idx = vecindex_create(dim, (vecindex_type_t)type,
                                      (vecindex_dist_t)dist);
    if (!idx) return NULL;

    if (type == VECINDEX_IVF && nlist > 0) {
        idx->nlist = nlist;
        idx->centroids = (int8_t *)kmalloc(nlist * dim);
        if (idx->centroids) {
            memcpy(idx->centroids, p, nlist * dim);
            p += nlist * dim;
        }
        idx->trained = true;
    }

    for (uint32_t i = 0; i < count; i++) {
        if ((size_t)(p - (const uint8_t *)buf) + 8 + dim > size) break;
        uint64_t id;
        memcpy(&id, p, 8); p += 8;
        vecindex_add(idx, id, (const int8_t *)p);
        p += dim;
    }

    return idx;
}
