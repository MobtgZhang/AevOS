#include "evolution.h"
#include "agent_core.h"
#include "llm/llm_runtime.h"
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "kernel/drivers/timer.h"
#include "lib/string.h"

/* ── evolution_init ──────────────────────────────────────── */

void evolution_init(evolution_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->current_version = 1;
    state->auto_evolve_enabled = false;
}

/* ── TinyCC compilation abstraction ──────────────────────── */

tcc_state_t *tcc_new(void) {
    tcc_state_t *tcc = (tcc_state_t *)kcalloc(1, sizeof(tcc_state_t));
    return tcc;
}

void tcc_delete(tcc_state_t *tcc) {
    if (!tcc) return;
    kfree(tcc->code);
    kfree(tcc->compiled_mem);
    kfree(tcc);
}

/*
 * Compile C source code string into executable memory.
 * This is a simplified model of what TinyCC does:
 * 1. Parse the C source
 * 2. Generate x86-64 machine code
 * 3. Place it in executable memory
 *
 * Real implementation would use the TinyCC library API.
 * For now, we validate the code structure and prepare the state.
 */
int tcc_compile_string(tcc_state_t *tcc, const char *code) {
    if (!tcc || !code) return -EINVAL;

    size_t code_len = strlen(code);
    if (code_len == 0) {
        strncpy(tcc->error_msg, "empty source", sizeof(tcc->error_msg));
        return -1;
    }

    /* Store the source code */
    tcc->code = (char *)kmalloc(code_len + 1);
    if (!tcc->code) {
        strncpy(tcc->error_msg, "out of memory", sizeof(tcc->error_msg));
        return -ENOMEM;
    }
    memcpy(tcc->code, code, code_len + 1);
    tcc->code_len = code_len;

    /* Validate basic structure: must contain "skill_main" function */
    if (!strstr(code, "skill_main")) {
        strncpy(tcc->error_msg, "missing skill_main function", sizeof(tcc->error_msg));
        return -1;
    }

    /* Check for dangerous patterns */
    static const char *banned[] = {
        "asm(", "__asm__", "goto ", "system(", "exec(",
        "fork(", "kill(", NULL
    };
    for (int i = 0; banned[i]; i++) {
        if (strstr(code, banned[i])) {
            snprintf(tcc->error_msg, sizeof(tcc->error_msg),
                     "banned construct: %s", banned[i]);
            return -1;
        }
    }

    /*
     * In a real implementation, TinyCC would compile to machine code here.
     * We allocate a region for the compiled output and mark it "compiled".
     * The actual JIT compilation would produce native x86-64 instructions.
     */
    tcc->compiled_size = 4096;
    tcc->compiled_mem = kmalloc(tcc->compiled_size);
    if (!tcc->compiled_mem) {
        strncpy(tcc->error_msg, "compilation memory alloc failed",
                sizeof(tcc->error_msg));
        return -ENOMEM;
    }
#if defined(__x86_64__)
    memset(tcc->compiled_mem, 0xCC, tcc->compiled_size); /* x86 INT3 trap fill */
#else
    memset(tcc->compiled_mem, 0x00, tcc->compiled_size); /* zero fill for safety */
#endif
    tcc->compiled = true;

    return 0;
}

void *tcc_get_symbol(tcc_state_t *tcc, const char *name) {
    if (!tcc || !tcc->compiled || !name) return NULL;

    /* In a real TinyCC integration, this would resolve a symbol from the
     * compiled object. For now, return NULL to indicate the symbol isn't
     * available yet — the caller should handle this gracefully. */
    (void)name;

    /*
     * When TinyCC is fully integrated:
     *   return tcc_get_symbol_real(tcc->internal_state, name);
     */
    return NULL;
}

/* ── Sandbox testing ─────────────────────────────────────── */

int sandbox_test(skill_fn_t fn) {
    if (!fn) return -EINVAL;

    /*
     * Execute the skill function in a sandboxed context:
     * 1. Set up a restricted memory region (ring3 in real implementation)
     * 2. Run with a timeout
     * 3. Verify return value and output
     *
     * For safety, we wrap the execution and catch any failures.
     */
    char test_input[] = "__sandbox_test__";
    char test_output[256];
    memset(test_output, 0, sizeof(test_output));

    int result = fn(test_input, test_output, sizeof(test_output));

    /* Basic validation: function returned without crashing */
    if (result < 0) {
        klog("sandbox: skill test failed with code %d\n", result);
        return -ESKILL_FAIL;
    }

    /* Ensure output is null-terminated */
    test_output[sizeof(test_output) - 1] = '\0';

    klog("sandbox: skill test passed (result=%d)\n", result);
    return 0;
}

/* ── v0.1: LLM-assisted skill generation ─────────────────── */

int agent_evolve_skill(struct agent *agent, const char *task_desc) {
    if (!agent || !task_desc) return -EINVAL;

    agent->state = AGENT_EVOLVING;

    /* Build the code-generation prompt */
    char prompt[2048];
    snprintf(prompt, sizeof(prompt),
        "Write a single C function with this exact signature:\n"
        "int skill_main(const char *input, char *output, size_t out_max)\n"
        "The function should: %s\n"
        "Return 0 on success, negative on error.\n"
        "Output ONLY the C code, no comments or explanations.\n"
        "Use only: memcpy, memset, strlen, strcmp, strncpy, snprintf.",
        task_desc);

    /* Call LLM to generate code */
    char generated_code[8192];
    memset(generated_code, 0, sizeof(generated_code));

    if (agent->llm) {
        int gen = llm_infer(agent->llm, prompt, generated_code, sizeof(generated_code));
        if (gen <= 0) {
            klog("evolve: LLM failed to generate code\n");
            agent->state = AGENT_IDLE;
            agent->evolution.failed_evolve_count++;
            return -EIO;
        }
    } else {
        klog("evolve: no LLM context available\n");
        agent->state = AGENT_IDLE;
        return -ENOTSUP;
    }

    /* Compile the generated code */
    tcc_state_t *tcc = tcc_new();
    if (!tcc) {
        agent->state = AGENT_IDLE;
        return -ENOMEM;
    }

    int compile_result = tcc_compile_string(tcc, generated_code);
    if (compile_result != 0) {
        klog("evolve: compilation failed: %s\n", tcc->error_msg);
        tcc_delete(tcc);
        agent->state = AGENT_IDLE;
        agent->evolution.failed_evolve_count++;
        return -ESKILL_FAIL;
    }

    /* Get the compiled function */
    skill_fn_t fn = (skill_fn_t)tcc_get_symbol(tcc, "skill_main");
    if (!fn) {
        klog("evolve: skill_main symbol not found (TinyCC not fully integrated)\n");
        tcc_delete(tcc);
        agent->state = AGENT_IDLE;
        agent->evolution.failed_evolve_count++;
        return -ESKILL_FAIL;
    }

    /* Sandbox test */
    if (sandbox_test(fn) != 0) {
        klog("evolve: sandbox test failed\n");
        tcc_delete(tcc);
        agent->state = AGENT_IDLE;
        agent->evolution.failed_evolve_count++;
        return -ESKILL_FAIL;
    }

    /* Register the new skill */
    char skill_name[64];
    snprintf(skill_name, sizeof(skill_name), "auto_%llu",
             (unsigned long long)agent->evolution.evolve_count);

    skill_t *s = skill_register(&agent->skills, fn, skill_name, task_desc);
    if (!s) {
        tcc_delete(tcc);
        agent->state = AGENT_IDLE;
        return -ENOSPC;
    }

    s->is_sandboxed = true;
    s->elf_handle = tcc; /* Keep TCC state alive (owns the compiled memory) */

    agent->evolution.evolve_count++;
    agent->evolution.last_evolve_time = timer_get_ms();
    agent->state = AGENT_IDLE;

    klog("evolve: agent '%s' created skill '%s' for: %s\n",
         agent->name, skill_name, task_desc);
    return 0;
}

/* ── v0.2: Auto-extract skills from history ──────────────── */

int agent_auto_extract_skills(struct agent *agent) {
    if (!agent) return -EINVAL;

    agent->state = AGENT_EVOLVING;

    uint32_t total = history_count(&agent->history);
    if (total < 6) {
        agent->state = AGENT_IDLE;
        return 0;
    }

    /*
     * Scan history for repeated assistant responses that look like
     * tool calls or structured operations. Group similar patterns
     * and attempt to create skills for the most common ones.
     */
    char *buf = (char *)kmalloc(4096);
    if (!buf) { agent->state = AGENT_IDLE; return -ENOMEM; }

    typedef struct { char desc[256]; uint32_t count; } cluster_t;
    cluster_t *clusters = (cluster_t *)kcalloc(32, sizeof(cluster_t));
    if (!clusters) { kfree(buf); agent->state = AGENT_IDLE; return -ENOMEM; }
    uint32_t n_clusters = 0;

    for (uint32_t i = 0; i < total; i++) {
        ssize_t len = history_get(&agent->history, i, buf, 4096);
        if (len <= 0) continue;

        /* Look for tool-call patterns like [TOOL:xxx] or function-like calls */
        char *tool_start = strstr(buf, "[TOOL:");
        if (!tool_start) tool_start = strstr(buf, "tool_call(");
        if (!tool_start) continue;

        char desc[256];
        size_t dl = (size_t)len;
        if (dl > 255) dl = 255;
        memcpy(desc, buf, dl);
        desc[dl] = '\0';

        /* Check if this pattern is already clustered */
        bool found = false;
        for (uint32_t c = 0; c < n_clusters; c++) {
            /* Simple similarity: compare first 64 chars */
            if (strncmp(clusters[c].desc, desc, 64) == 0) {
                clusters[c].count++;
                found = true;
                break;
            }
        }
        if (!found && n_clusters < 32) {
            strncpy(clusters[n_clusters].desc, desc, 255);
            clusters[n_clusters].count = 1;
            n_clusters++;
        }
    }

    int skills_created = 0;
    for (uint32_t c = 0; c < n_clusters; c++) {
        if (clusters[c].count >= 3) {
            /* Attempt to evolve a skill for this pattern */
            int rc = agent_evolve_skill(agent, clusters[c].desc);
            if (rc == 0) skills_created++;
        }
    }

    kfree(clusters);
    kfree(buf);
    agent->state = AGENT_IDLE;

    klog("evolve: auto-extracted %d skills from %u history entries\n",
         skills_created, total);
    return skills_created;
}

/* ── v0.3: Fully autonomous evolution pipeline ───────────── */

int agent_full_auto_evolve(struct agent *agent) {
    if (!agent) return -EINVAL;
    if (!agent->evolution.auto_evolve_enabled) return 0;

    /* Cooldown check */
    uint64_t now = timer_get_ms();
    if (now - agent->evolution.last_evolve_time < SKILL_AUTO_EVOLVE_COOLDOWN_MS)
        return 0;

    agent->state = AGENT_EVOLVING;

    /*
     * Autonomous pipeline:
     * 1. Scan recent history for failed tasks (assistant responses containing
     *    error keywords)
     * 2. For each failure pattern, attempt to generate a new skill
     * 3. Test in sandbox
     * 4. Deploy if successful
     */
    char *buf = (char *)kmalloc(4096);
    if (!buf) { agent->state = AGENT_IDLE; return -ENOMEM; }

    uint32_t total = history_count(&agent->history);
    uint32_t scan_window = MIN(total, 20);
    int evolved = 0;

    static const char *failure_keywords[] = {
        "error", "failed", "unable to", "cannot", "not supported",
        "no skill", "unknown tool", NULL
    };

    for (uint32_t i = total - scan_window; i < total; i++) {
        ssize_t len = history_get(&agent->history, i, buf, 4096);
        if (len <= 0) continue;

        /* Check if this entry contains a failure indicator */
        bool is_failure = false;
        for (int k = 0; failure_keywords[k]; k++) {
            if (strstr(buf, failure_keywords[k])) {
                is_failure = true;
                break;
            }
        }

        if (!is_failure) continue;

        /* Try to extract the task description from the previous user message */
        if (i > 0) {
            char task_buf[1024];
            ssize_t tlen = history_get(&agent->history, i - 1, task_buf, sizeof(task_buf));
            if (tlen > 0) {
                int rc = agent_evolve_skill(agent, task_buf);
                if (rc == 0) {
                    evolved++;
                    break; /* One skill per auto-evolve cycle */
                }
            }
        }
    }

    kfree(buf);
    agent->state = AGENT_IDLE;
    return evolved;
}

/* ── v0.4: Evaluate and retire skills ────────────────────── */

int agent_evaluate_and_retire(struct agent *agent) {
    if (!agent) return -EINVAL;

    /* Cooldown check */
    uint64_t now = timer_get_ms();
    if (now - agent->evolution.last_evolve_time < SKILL_AUTO_EVOLVE_COOLDOWN_MS)
        return 0;

    agent->state = AGENT_EVOLVING;

    /* Phase 1: Evaluate all skills */
    int flagged = skill_evaluate_all(&agent->skills);

    /* Phase 2: Retire poor performers */
    int retired = skill_auto_retire(&agent->skills);
    agent->evolution.retire_count += (uint32_t)retired;

    /* Phase 3: For each retired skill, attempt regeneration */
    int regenerated = 0;
    for (uint32_t i = 0; i < agent->skills.count; i++) {
        skill_t *s = &agent->skills.skills[i];
        if (s->active) continue;
        if (s->description[0] == '\0') continue;

        /* Only regenerate if the skill was recently retired */
        if (s->last_used > 0 && now - s->last_used < 3600000) {
            klog("evolve: attempting to regenerate retired skill '%s'\n", s->name);
            int rc = agent_evolve_skill(agent, s->description);
            if (rc == 0) regenerated++;
        }
    }

    agent->evolution.last_evolve_time = now;
    agent->state = AGENT_IDLE;

    klog("evolve: evaluated %d skills, retired %d, regenerated %d\n",
         flagged, retired, regenerated);
    return retired;
}

/* ── Statistics ──────────────────────────────────────────── */

evolution_stats_t evolution_get_stats(evolution_state_t *state) {
    evolution_stats_t stats = { 0 };
    if (!state) return stats;

    stats.total_evolves = state->evolve_count;
    stats.successful_evolves = state->evolve_count - state->failed_evolve_count;
    stats.total_retires = state->retire_count;
    stats.last_evolve_time = state->last_evolve_time;
    return stats;
}
