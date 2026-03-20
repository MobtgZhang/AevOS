#include "agent_core.h"
#include "llm/llm_runtime.h"
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "kernel/drivers/timer.h"
#include "kernel/sched/coroutine.h"
#include "lib/string.h"

#define AGENT_RESPONSE_BUF_SIZE 16384
#define AGENT_PROMPT_BUF_SIZE   32768

/* ── Agent System ────────────────────────────────────────── */

int agent_system_init(agent_system_t *sys) {
    if (!sys) return -EINVAL;
    memset(sys, 0, sizeof(*sys));
    sys->next_id = 1;
    sys->lock = SPINLOCK_INIT;
    klog("agent_system: initialized (max=%u)\n", MAX_AGENTS);
    return 0;
}

void agent_system_destroy(agent_system_t *sys) {
    if (!sys) return;
    spin_lock(&sys->lock);
    for (uint32_t i = 0; i < sys->count; i++) {
        if (sys->agents[i]) {
            agent_destroy(sys, sys->agents[i]);
            sys->agents[i] = NULL;
        }
    }
    sys->count = 0;
    spin_unlock(&sys->lock);
}

/* ── Agent Lifecycle ─────────────────────────────────────── */

agent_t *agent_create(agent_system_t *sys, const char *name) {
    if (!sys || !name) return NULL;

    spin_lock(&sys->lock);
    if (sys->count >= MAX_AGENTS) {
        spin_unlock(&sys->lock);
        klog("agent: max agents reached\n");
        return NULL;
    }

    agent_t *agent = (agent_t *)kcalloc(1, sizeof(agent_t));
    if (!agent) { spin_unlock(&sys->lock); return NULL; }

    strncpy(agent->name, name, sizeof(agent->name) - 1);
    agent->id = sys->next_id++;
    agent->created_at = timer_get_ms();
    agent->state = AGENT_IDLE;

    /* Initialize sub-engines */
    history_init(&agent->history, LLM_DEFAULT_CTX);

    if (memory_init(&agent->memory) < 0) {
        kfree(agent);
        spin_unlock(&sys->lock);
        return NULL;
    }

    skill_init(&agent->skills);
    evolution_init(&agent->evolution);

    /* Allocate response buffer */
    agent->response_buf_size = AGENT_RESPONSE_BUF_SIZE;
    agent->response_buf = (char *)kmalloc(AGENT_RESPONSE_BUF_SIZE);
    if (!agent->response_buf) {
        memory_destroy(&agent->memory);
        kfree(agent);
        spin_unlock(&sys->lock);
        return NULL;
    }

    sys->agents[sys->count++] = agent;
    if (sys->count == 1)
        sys->active_agent = 0;

    spin_unlock(&sys->lock);
    klog("agent: created '%s' (id=%llu)\n", name, (unsigned long long)agent->id);
    return agent;
}

void agent_destroy(agent_system_t *sys, agent_t *agent) {
    if (!agent) return;

    klog("agent: destroying '%s'\n", agent->name);

    history_clear(&agent->history);
    memory_destroy(&agent->memory);
    skill_destroy(&agent->skills);

    kfree(agent->response_buf);

    /* Remove from system array */
    if (sys) {
        spin_lock(&sys->lock);
        for (uint32_t i = 0; i < sys->count; i++) {
            if (sys->agents[i] == agent) {
                for (uint32_t j = i; j < sys->count - 1; j++)
                    sys->agents[j] = sys->agents[j + 1];
                sys->agents[sys->count - 1] = NULL;
                sys->count--;
                break;
            }
        }
        spin_unlock(&sys->lock);
    }

    kfree(agent);
}

/* ── Active agent management ─────────────────────────────── */

void agent_set_active(agent_system_t *sys, uint64_t agent_id) {
    if (!sys) return;
    spin_lock(&sys->lock);
    for (uint32_t i = 0; i < sys->count; i++) {
        if (sys->agents[i] && sys->agents[i]->id == agent_id) {
            sys->active_agent = i;
            klog("agent: active set to '%s'\n", sys->agents[i]->name);
            break;
        }
    }
    spin_unlock(&sys->lock);
}

agent_t *agent_get_active(agent_system_t *sys) {
    if (!sys || sys->count == 0) return NULL;
    if (sys->active_agent >= sys->count) return NULL;
    return sys->agents[sys->active_agent];
}

/* ── Build prompt from history + memory context ──────────── */

static int build_prompt(agent_t *agent, char *prompt_buf, size_t prompt_max) {
    size_t pos = 0;

    /* System prompt */
    pos += (size_t)snprintf(prompt_buf + pos, prompt_max - pos,
        "<|system|>\nYou are %s, an AI agent running on AevOS. "
        "You have skills you can call using [TOOL:skill_name(args)].\n"
        "Available skills:\n", agent->name);

    /* List active skills */
    for (uint32_t i = 0; i < agent->skills.count && pos < prompt_max - 256; i++) {
        skill_t *s = &agent->skills.skills[i];
        if (!s->active) continue;
        pos += (size_t)snprintf(prompt_buf + pos, prompt_max - pos,
            "- %s: %s\n", s->name, s->description);
    }

    /* Retrieve relevant memories for context */
    /* (Would use memory_retrieve with the last user message's embedding,
     *  but for now we include a brief memory summary) */
    if (agent->memory.count > 0) {
        pos += (size_t)snprintf(prompt_buf + pos, prompt_max - pos,
            "\nMemory context (%u entries available):\n", agent->memory.count);
    }

    pos += (size_t)snprintf(prompt_buf + pos, prompt_max - pos,
        "<|end_system|>\n");

    /* Replay history as conversation turns */
    uint32_t count = history_count(&agent->history);
    char entry_buf[2048];

    for (uint32_t i = 0; i < count && pos < prompt_max - 512; i++) {
        ssize_t len = history_get(&agent->history, i, entry_buf, sizeof(entry_buf));
        if (len <= 0) continue;

        uint32_t dummy;
        hist_entry_t *entries = history_get_recent(&agent->history, count, &dummy);
        if (!entries) continue;

        uint8_t role = entries[i < dummy ? i : 0].role;
        kfree(entries);

        const char *role_tag = "<|user|>";
        switch (role) {
        case HIST_ROLE_ASSISTANT: role_tag = "<|assistant|>"; break;
        case HIST_ROLE_SYSTEM:    role_tag = "<|system|>";    break;
        case HIST_ROLE_TOOL:      role_tag = "<|tool|>";      break;
        }

        pos += (size_t)snprintf(prompt_buf + pos, prompt_max - pos,
            "%s\n%s\n", role_tag, entry_buf);
    }

    pos += (size_t)snprintf(prompt_buf + pos, prompt_max - pos,
        "<|assistant|>\n");

    return (int)pos;
}

/* ── Parse response for tool calls ───────────────────────── */

static int parse_and_execute_tools(agent_t *agent, const char *response,
                                   char *tool_output, size_t tool_max) {
    /* Look for [TOOL:skill_name(args)] patterns */
    const char *p = response;
    int calls = 0;
    size_t out_pos = 0;

    while ((p = strstr(p, "[TOOL:")) != NULL) {
        p += 6; /* skip "[TOOL:" */

        /* Extract skill name */
        char skill_name[64];
        int ni = 0;
        while (*p && *p != '(' && *p != ']' && ni < 63)
            skill_name[ni++] = *p++;
        skill_name[ni] = '\0';

        /* Extract arguments */
        char args[1024] = "";
        if (*p == '(') {
            p++;
            int ai = 0;
            int depth = 1;
            while (*p && depth > 0 && ai < 1023) {
                if (*p == '(') depth++;
                if (*p == ')') { depth--; if (depth == 0) break; }
                args[ai++] = *p++;
            }
            args[ai] = '\0';
            if (*p == ')') p++;
        }
        if (*p == ']') p++;

        /* Find and execute the skill */
        skill_t *skill = skill_find_by_name(&agent->skills, skill_name);
        if (skill) {
            char result[2048];
            int rc = skill_execute(&agent->skills, skill->id, args,
                                   result, sizeof(result));

            size_t rlen = strlen(result);
            if (out_pos + rlen + 64 < tool_max) {
                out_pos += (size_t)snprintf(tool_output + out_pos,
                    tool_max - out_pos,
                    "[RESULT:%s] %s\n",
                    rc == 0 ? "OK" : "FAIL",
                    result);
            }
            calls++;
        } else {
            if (out_pos + 128 < tool_max) {
                out_pos += (size_t)snprintf(tool_output + out_pos,
                    tool_max - out_pos,
                    "[RESULT:FAIL] Unknown skill '%s'\n", skill_name);
            }
        }
    }

    return calls;
}

/* ── Main processing pipeline ────────────────────────────── */

const char *agent_process_input(agent_t *agent, const char *user_text) {
    if (!agent || !user_text) return NULL;

    agent->state = AGENT_THINKING;

    /* Push user input to history */
    history_push(&agent->history, HIST_ROLE_USER, user_text);

    /* Build the full prompt */
    char *prompt_buf = (char *)kmalloc(AGENT_PROMPT_BUF_SIZE);
    if (!prompt_buf) {
        agent->state = AGENT_IDLE;
        strncpy(agent->response_buf, "[Error: out of memory]",
                agent->response_buf_size);
        return agent->response_buf;
    }

    build_prompt(agent, prompt_buf, AGENT_PROMPT_BUF_SIZE);

    /* Call LLM inference */
    memset(agent->response_buf, 0, agent->response_buf_size);

    if (agent->llm) {
        int gen = llm_infer(agent->llm, prompt_buf,
                            agent->response_buf, agent->response_buf_size);
        if (gen < 0) {
            snprintf(agent->response_buf, agent->response_buf_size,
                     "[LLM inference error: %d]", gen);
        }
    } else {
        snprintf(agent->response_buf, agent->response_buf_size,
                 "[No LLM loaded — agent '%s' ready for configuration]",
                 agent->name);
    }

    kfree(prompt_buf);

    /* Parse response for tool calls */
    agent->state = AGENT_EXECUTING;

    char *tool_output = (char *)kmalloc(4096);
    if (tool_output) {
        memset(tool_output, 0, 4096);
        int tool_calls = parse_and_execute_tools(agent, agent->response_buf,
                                                 tool_output, 4096);
        if (tool_calls > 0) {
            /* Append tool results to history and optionally re-infer */
            history_push(&agent->history, HIST_ROLE_TOOL, tool_output);
        }
        kfree(tool_output);
    }

    /* Push assistant response to history */
    history_push(&agent->history, HIST_ROLE_ASSISTANT, agent->response_buf);

    agent->state = AGENT_IDLE;
    return agent->response_buf;
}

/* ── Periodic maintenance tick ───────────────────────────── */

void agent_tick(agent_t *agent) {
    if (!agent || agent->state != AGENT_IDLE) return;

    /* Memory decay (forgetting curve) */
    memory_forget(&agent->memory);

    /* Skill evaluation (v0.4) */
    skill_evaluate_all(&agent->skills);

    /* Auto-evolution check (v0.3+) */
    if (agent->evolution.auto_evolve_enabled) {
        agent_full_auto_evolve(agent);
    }
}

/* ── Persistence ─────────────────────────────────────────── */

#define AGENT_SAVE_MAGIC 0x41474E54u /* "AGNT" */

int agent_save_state(agent_t *agent, const char *path) {
    if (!agent || !path) return -EINVAL;

    /* Calculate buffer sizes needed for each component */
    ssize_t hist_need = history_serialize(&agent->history, NULL, 0);
    if (hist_need < 0) hist_need = 0;

    ssize_t mem_need = memory_serialize(&agent->memory, NULL, 0);
    if (mem_need < 0) mem_need = 0;

    ssize_t skill_need = skill_serialize(&agent->skills, NULL, 0);
    if (skill_need < 0) skill_need = 0;

    /* Total: header + name + evolution state + components */
    size_t total = 4 + 4 + 64 + sizeof(evolution_state_t)
                 + 4 + (size_t)hist_need
                 + 4 + (size_t)mem_need
                 + 4 + (size_t)skill_need;

    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (!buf) return -ENOMEM;

    uint8_t *p = buf;

    /* Header */
    uint32_t magic = AGENT_SAVE_MAGIC;
    memcpy(p, &magic, 4); p += 4;

    uint32_t version = 1;
    memcpy(p, &version, 4); p += 4;

    /* Agent name */
    memcpy(p, agent->name, 64); p += 64;

    /* Evolution state */
    memcpy(p, &agent->evolution, sizeof(evolution_state_t));
    p += sizeof(evolution_state_t);

    /* History */
    uint32_t h_size = (uint32_t)hist_need;
    memcpy(p, &h_size, 4); p += 4;
    if (h_size > 0) {
        history_serialize(&agent->history, p, h_size);
        p += h_size;
    }

    /* Memory */
    uint32_t m_size = (uint32_t)mem_need;
    memcpy(p, &m_size, 4); p += 4;
    if (m_size > 0) {
        memory_serialize(&agent->memory, p, m_size);
        p += m_size;
    }

    /* Skills */
    uint32_t s_size = (uint32_t)skill_need;
    memcpy(p, &s_size, 4); p += 4;
    if (s_size > 0) {
        skill_serialize(&agent->skills, p, s_size);
        p += s_size;
    }

    /*
     * In a real implementation, we'd write `buf` to the filesystem:
     *   int fd = vfs_open(path, O_WRONLY | O_CREAT);
     *   vfs_write(fd, buf, total);
     *   vfs_close(fd);
     */
    klog("agent: saved state for '%s' (%llu bytes) to %s\n",
         agent->name, (unsigned long long)(p - buf), path);

    kfree(buf);
    return 0;
}

int agent_load_state(agent_t *agent, const char *path) {
    if (!agent || !path) return -EINVAL;

    /*
     * In a real implementation, we'd read from the filesystem:
     *   int fd = vfs_open(path, O_RDONLY);
     *   size_t sz = vfs_size(fd);
     *   uint8_t *buf = kmalloc(sz);
     *   vfs_read(fd, buf, sz);
     *   vfs_close(fd);
     *
     * For now, return an error since VFS is not yet available.
     */
    klog("agent: load_state not yet available (VFS pending) for '%s'\n", path);
    (void)agent;
    return -ENOTSUP;
}
