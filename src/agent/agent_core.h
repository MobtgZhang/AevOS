#pragma once

#include <aevos/types.h>
#include <aevos/config.h>
#include "history.h"
#include "memory.h"
#include "skill.h"
#include "evolution.h"

struct llm_ctx;

typedef enum {
    AGENT_IDLE      = 0,
    AGENT_THINKING  = 1,
    AGENT_EXECUTING = 2,
    AGENT_EVOLVING  = 3,
} agent_state_t;

typedef struct agent {
    history_t         history;
    memory_engine_t   memory;
    skill_engine_t    skills;
    struct llm_ctx   *llm;
    void             *main_coro;
    char              name[64];
    uint64_t          created_at;
    uint64_t          id;
    agent_state_t     state;
    evolution_state_t evolution;
    char             *response_buf;
    size_t            response_buf_size;
} agent_t;

typedef struct {
    agent_t   *agents[MAX_AGENTS];
    uint32_t   count;
    uint32_t   active_agent;
    uint64_t   next_id;
    spinlock_t lock;
} agent_system_t;

int        agent_system_init(agent_system_t *sys);
void       agent_system_destroy(agent_system_t *sys);

agent_t   *agent_create(agent_system_t *sys, const char *name);
void       agent_destroy(agent_system_t *sys, agent_t *agent);

const char *agent_process_input(agent_t *agent, const char *user_text);
void        agent_set_active(agent_system_t *sys, uint64_t agent_id);
agent_t    *agent_get_active(agent_system_t *sys);

void agent_tick(agent_t *agent);

int agent_save_state(agent_t *agent, const char *path);
int agent_load_state(agent_t *agent, const char *path);
