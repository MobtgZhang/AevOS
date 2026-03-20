#pragma once

#include <aevos/types.h>
#include <aevos/config.h>
#include "skill.h"

struct agent;

typedef struct {
    uint32_t current_version;
    bool     auto_evolve_enabled;
    uint64_t last_evolve_time;
    uint32_t evolve_count;
    uint32_t retire_count;
    uint32_t failed_evolve_count;
} evolution_state_t;

typedef struct {
    char    *code;
    size_t   code_len;
    void    *compiled_mem;
    size_t   compiled_size;
    bool     compiled;
    char     error_msg[256];
} tcc_state_t;

typedef struct {
    uint32_t total_evolves;
    uint32_t successful_evolves;
    uint32_t total_retires;
    uint64_t last_evolve_time;
} evolution_stats_t;

void evolution_init(evolution_state_t *state);

int agent_evolve_skill(struct agent *agent, const char *task_desc);
int agent_auto_extract_skills(struct agent *agent);
int agent_full_auto_evolve(struct agent *agent);
int agent_evaluate_and_retire(struct agent *agent);

evolution_stats_t evolution_get_stats(evolution_state_t *state);

tcc_state_t *tcc_new(void);
void         tcc_delete(tcc_state_t *tcc);
int          tcc_compile_string(tcc_state_t *tcc, const char *code);
void        *tcc_get_symbol(tcc_state_t *tcc, const char *name);
int          sandbox_test(skill_fn_t fn);
