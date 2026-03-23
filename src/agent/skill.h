#pragma once

#include <aevos/types.h>
#include <aevos/config.h>
#include "lib/hashmap.h"

typedef int (*skill_fn_t)(const char *input, char *output, size_t out_max);

typedef struct {
    float    success_rate;
    uint32_t call_count;
    uint32_t fail_count;
    uint64_t total_latency_us;
    uint64_t avg_latency_us;
} skill_stats_t;

typedef struct {
    uint64_t    id;
    char        name[64];
    char        description[256];
    char        signature[128];
    skill_fn_t  fn;
    float       success_rate;
    uint32_t    call_count;
    uint32_t    fail_count;
    uint64_t    total_latency_us;
    uint64_t    avg_latency_us;
    bool        is_sandboxed;
    void       *elf_handle;
    uint64_t    created_at;
    uint64_t    last_used;
    uint32_t    version;
    bool        active;
} skill_t;

typedef struct {
    uint32_t deps[16];
    uint32_t n_deps;
} skill_dep_node_t;

typedef struct {
    skill_t          skills[MAX_SKILLS];
    uint32_t         count;
    uint64_t         next_id;
    skill_dep_node_t dep_graph[MAX_SKILLS];
    hashmap_t       *registry_by_name;
    spinlock_t       lock;
} skill_engine_t;

struct history;

void     skill_init(skill_engine_t *engine);
void     skill_destroy(skill_engine_t *engine);

skill_t *skill_register(skill_engine_t *engine, skill_fn_t fn,
                        const char *name, const char *description);
int      skill_unregister(skill_engine_t *engine, uint64_t skill_id);
skill_t *skill_find_by_name(skill_engine_t *engine, const char *name);
skill_t *skill_find_by_id(skill_engine_t *engine, uint64_t skill_id);

int skill_execute(skill_engine_t *engine, uint64_t skill_id,
                  const char *input, char *output, size_t out_max);

skill_t      *skill_list(skill_engine_t *engine, uint32_t *out_count);
skill_stats_t skill_get_stats(skill_engine_t *engine, uint64_t skill_id);

int skill_extract_from_history(skill_engine_t *engine, struct history *hist);

int skill_evaluate_all(skill_engine_t *engine);
int skill_auto_retire(skill_engine_t *engine);

ssize_t skill_serialize(skill_engine_t *engine, void *buf, size_t size);
int     skill_deserialize(skill_engine_t *engine, const void *buf, size_t size);
