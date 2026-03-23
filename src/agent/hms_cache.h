#pragma once

#include <aevos/types.h>

/*
 * HMS Memory-side cache hierarchy (类比 CPU→内存→外存):
 *   L1 — 极小、最热（CLOCK）
 *   L2 — 较大、温数据（LRU 近似）
 *   L3 — 冷侧摘要（ANN 查询结果的压缩快照，命中则避免完整 HNSW 遍历）
 * History B+ / Skill 注册表在各自模块；此处专注 Memory 检索路径。
 */
typedef enum {
    HMS_MEM_TIER_NONE  = 0,
    HMS_MEM_TIER_L1    = 1,
    HMS_MEM_TIER_L2    = 2,
    HMS_MEM_TIER_L3    = 3,
    HMS_MEM_TIER_HNSW  = 4,
} hms_mem_tier_t;

typedef struct {
    uint32_t l1_slots;
    uint32_t l2_slots;
    uint32_t l3_slots;
    uint32_t l1_used;
    uint32_t l2_used;
    uint32_t l3_used;
    uint64_t hits_l1;
    uint64_t hits_l2;
    uint64_t hits_l3;
    uint64_t misses;
    void    *l1_private;
    void    *l2_private;
    void    *l3_private;
    spinlock_t lock;
} hms_cache_t;

void hms_cache_init(hms_cache_t *c, uint32_t l1_slots);
void hms_cache_destroy(hms_cache_t *c);

int  hms_cache_l1_get(hms_cache_t *c, const char *key, void *out, size_t *io_len);
int  hms_cache_l1_put(hms_cache_t *c, const char *key, const void *data, size_t len);

int  hms_cache_l2_get(hms_cache_t *c, const char *key, void *out, size_t *io_len);
int  hms_cache_l2_put(hms_cache_t *c, const char *key, const void *data, size_t len);

int  hms_cache_l3_get(hms_cache_t *c, const char *key, void *out, size_t *io_len);
int  hms_cache_l3_put(hms_cache_t *c, const char *key, const void *data, size_t len);
