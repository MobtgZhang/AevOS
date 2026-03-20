#include "skill.h"
#include "history.h"
#include "kernel/mm/slab.h"
#include "kernel/klog.h"
#include "kernel/drivers/timer.h"
#include "lib/string.h"

/* ── skill_init ──────────────────────────────────────────── */

void skill_init(skill_engine_t *engine) {
    if (!engine) return;
    memset(engine, 0, sizeof(*engine));
    engine->next_id = 1;
    engine->lock = SPINLOCK_INIT;
}

void skill_destroy(skill_engine_t *engine) {
    if (!engine) return;
    spin_lock(&engine->lock);
    for (uint32_t i = 0; i < engine->count; i++) {
        if (engine->skills[i].elf_handle)
            kfree(engine->skills[i].elf_handle);
    }
    engine->count = 0;
    spin_unlock(&engine->lock);
}

/* ── Registration ────────────────────────────────────────── */

skill_t *skill_register(skill_engine_t *engine, skill_fn_t fn,
                        const char *name, const char *description) {
    if (!engine || !fn || !name) return NULL;

    spin_lock(&engine->lock);

    if (engine->count >= MAX_SKILLS) {
        spin_unlock(&engine->lock);
        klog("skill: max skills reached (%u)\n", MAX_SKILLS);
        return NULL;
    }

    /* Check for name collision */
    for (uint32_t i = 0; i < engine->count; i++) {
        if (engine->skills[i].active && strcmp(engine->skills[i].name, name) == 0) {
            spin_unlock(&engine->lock);
            klog("skill: '%s' already registered\n", name);
            return NULL;
        }
    }

    skill_t *s = &engine->skills[engine->count];
    memset(s, 0, sizeof(*s));

    s->id = engine->next_id++;
    strncpy(s->name, name, sizeof(s->name) - 1);
    if (description)
        strncpy(s->description, description, sizeof(s->description) - 1);
    snprintf(s->signature, sizeof(s->signature),
             "int %s(const char*, char*, size_t)", name);
    s->fn = fn;
    s->success_rate = 1.0f;
    s->created_at = timer_get_ms();
    s->version = 1;
    s->active = true;

    engine->count++;
    spin_unlock(&engine->lock);

    klog("skill: registered '%s' (id=%llu)\n", name, (unsigned long long)s->id);
    return s;
}

int skill_unregister(skill_engine_t *engine, uint64_t skill_id) {
    if (!engine) return -EINVAL;

    spin_lock(&engine->lock);

    for (uint32_t i = 0; i < engine->count; i++) {
        if (engine->skills[i].id == skill_id) {
            klog("skill: unregistered '%s'\n", engine->skills[i].name);
            if (engine->skills[i].elf_handle)
                kfree(engine->skills[i].elf_handle);
            engine->skills[i].active = false;
            engine->skills[i].fn = NULL;
            spin_unlock(&engine->lock);
            return 0;
        }
    }

    spin_unlock(&engine->lock);
    return -ENOENT;
}

skill_t *skill_find_by_name(skill_engine_t *engine, const char *name) {
    if (!engine || !name) return NULL;
    for (uint32_t i = 0; i < engine->count; i++) {
        if (engine->skills[i].active && strcmp(engine->skills[i].name, name) == 0)
            return &engine->skills[i];
    }
    return NULL;
}

skill_t *skill_find_by_id(skill_engine_t *engine, uint64_t skill_id) {
    if (!engine) return NULL;
    for (uint32_t i = 0; i < engine->count; i++) {
        if (engine->skills[i].id == skill_id && engine->skills[i].active)
            return &engine->skills[i];
    }
    return NULL;
}

/* ── Execution with timing and stats ─────────────────────── */

int skill_execute(skill_engine_t *engine, uint64_t skill_id,
                  const char *input, char *output, size_t out_max) {
    if (!engine || !input || !output) return -EINVAL;

    skill_t *s = skill_find_by_id(engine, skill_id);
    if (!s || !s->fn) return -ENOENT;

    uint64_t start = timer_get_ms();
    int result = s->fn(input, output, out_max);
    uint64_t elapsed_us = (timer_get_ms() - start) * 1000;

    spin_lock(&engine->lock);

    s->call_count++;
    s->total_latency_us += elapsed_us;
    s->avg_latency_us = s->total_latency_us / s->call_count;
    s->last_used = timer_get_ms();

    if (result == 0) {
        /* Success — update running average */
        s->success_rate = (s->success_rate * (float)(s->call_count - 1) + 1.0f)
                        / (float)s->call_count;
    } else {
        s->fail_count++;
        s->success_rate = (s->success_rate * (float)(s->call_count - 1))
                        / (float)s->call_count;
    }

    spin_unlock(&engine->lock);
    return result;
}

/* ── Query ───────────────────────────────────────────────── */

skill_t *skill_list(skill_engine_t *engine, uint32_t *out_count) {
    if (!engine) { if (out_count) *out_count = 0; return NULL; }

    uint32_t active = 0;
    for (uint32_t i = 0; i < engine->count; i++)
        if (engine->skills[i].active) active++;

    if (out_count) *out_count = active;
    return engine->skills;
}

skill_stats_t skill_get_stats(skill_engine_t *engine, uint64_t skill_id) {
    skill_stats_t st = { 0 };
    skill_t *s = skill_find_by_id(engine, skill_id);
    if (!s) return st;

    st.success_rate     = s->success_rate;
    st.call_count       = s->call_count;
    st.fail_count       = s->fail_count;
    st.total_latency_us = s->total_latency_us;
    st.avg_latency_us   = s->avg_latency_us;
    return st;
}

/* ── v0.2: Extract skills from history ───────────────────── */

int skill_extract_from_history(skill_engine_t *engine, struct history *hist) {
    if (!engine || !hist) return -EINVAL;

    uint32_t total = history_count(hist);
    if (total < 4) return 0;

    /*
     * Strategy: scan history for repeated tool-call patterns.
     * If we see the same tool call with role == HIST_ROLE_TOOL
     * appearing >= 3 times, it's a candidate for skill extraction.
     *
     * We collect unique tool-call texts and count occurrences.
     */
    char *buf = (char *)kmalloc(4096);
    if (!buf) return -ENOMEM;

    typedef struct { char sig[128]; uint32_t count; } pattern_t;
    uint32_t max_patterns = 64;
    pattern_t *patterns = (pattern_t *)kcalloc(max_patterns, sizeof(pattern_t));
    if (!patterns) { kfree(buf); return -ENOMEM; }
    uint32_t n_patterns = 0;

    for (uint32_t i = 0; i < total; i++) {
        ssize_t len = history_get(hist, i, buf, 4096);
        if (len <= 0) continue;

        uint32_t abs_idx = i;  /* logical index */
        /* We look at the raw ring to check role — use history_get_recent */
        uint32_t dummy;
        hist_entry_t *recent = history_get_recent(hist, total, &dummy);
        if (!recent) continue;

        if (abs_idx < dummy && recent[abs_idx].role == HIST_ROLE_TOOL) {
            /* Extract first 127 chars as signature */
            char sig[128];
            size_t sl = (size_t)len;
            if (sl > 127) sl = 127;
            memcpy(sig, buf, sl);
            sig[sl] = '\0';

            /* Check if pattern already recorded */
            bool found = false;
            for (uint32_t p = 0; p < n_patterns; p++) {
                if (strcmp(patterns[p].sig, sig) == 0) {
                    patterns[p].count++;
                    found = true;
                    break;
                }
            }
            if (!found && n_patterns < max_patterns) {
                strncpy(patterns[n_patterns].sig, sig, 127);
                patterns[n_patterns].count = 1;
                n_patterns++;
            }
        }
        kfree(recent);
    }

    int extracted = 0;
    for (uint32_t p = 0; p < n_patterns; p++) {
        if (patterns[p].count >= 3) {
            klog("skill: pattern found (%u occurrences): %.64s...\n",
                 patterns[p].count, patterns[p].sig);
            extracted++;
        }
    }

    kfree(patterns);
    kfree(buf);
    return extracted;
}

/* ── v0.4: Evaluate all skills ───────────────────────────── */

int skill_evaluate_all(skill_engine_t *engine) {
    if (!engine) return -EINVAL;

    int flagged = 0;
    spin_lock(&engine->lock);

    for (uint32_t i = 0; i < engine->count; i++) {
        skill_t *s = &engine->skills[i];
        if (!s->active) continue;
        if (s->call_count < SKILL_MIN_CALLS_EVAL) continue;

        if (s->success_rate < SKILL_SUCCESS_THRESHOLD) {
            klog("skill: '%s' flagged for retirement (rate=%.2f%%, calls=%u)\n",
                 s->name, s->success_rate * 100.0f, s->call_count);
            flagged++;
        }
    }

    spin_unlock(&engine->lock);
    return flagged;
}

/* ── v0.4: Auto-retire poor skills ───────────────────────── */

int skill_auto_retire(skill_engine_t *engine) {
    if (!engine) return -EINVAL;

    int retired = 0;
    spin_lock(&engine->lock);

    for (uint32_t i = 0; i < engine->count; i++) {
        skill_t *s = &engine->skills[i];
        if (!s->active) continue;
        if (s->call_count < SKILL_MIN_CALLS_EVAL) continue;

        if (s->success_rate < SKILL_SUCCESS_THRESHOLD) {
            klog("skill: retiring '%s' (rate=%.2f%%)\n",
                 s->name, s->success_rate * 100.0f);
            s->active = false;
            s->fn = NULL;
            if (s->elf_handle) { kfree(s->elf_handle); s->elf_handle = NULL; }
            retired++;
        }
    }

    spin_unlock(&engine->lock);
    return retired;
}

/* ── Serialization ───────────────────────────────────────── */

#define SKILL_SERIAL_MAGIC 0x534B4C53u /* "SKLS" */

ssize_t skill_serialize(skill_engine_t *engine, void *buf, size_t size) {
    if (!engine || !buf) return -EINVAL;

    spin_lock(&engine->lock);

    /* Count active skills */
    uint32_t active = 0;
    for (uint32_t i = 0; i < engine->count; i++)
        if (engine->skills[i].active) active++;

    /* Per skill: id(8) + name(64) + desc(256) + sig(128) + stats(28)
     *            + flags(8) + times(24) = ~516 bytes */
    size_t per_skill = 8 + 64 + 256 + 128 + 4 + 4 + 4 + 8 + 8 + 8 + 4 + 4;
    size_t needed = 12 + active * per_skill;

    if (size < needed) { spin_unlock(&engine->lock); return (ssize_t)needed; }

    uint8_t *p = (uint8_t *)buf;
    uint32_t magic = SKILL_SERIAL_MAGIC;
    memcpy(p, &magic, 4);   p += 4;
    memcpy(p, &active, 4);  p += 4;
    memcpy(p, &engine->next_id, 8); p += 8;

    for (uint32_t i = 0; i < engine->count; i++) {
        skill_t *s = &engine->skills[i];
        if (!s->active) continue;

        memcpy(p, &s->id, 8);               p += 8;
        memcpy(p, s->name, 64);             p += 64;
        memcpy(p, s->description, 256);     p += 256;
        memcpy(p, s->signature, 128);       p += 128;
        memcpy(p, &s->success_rate, 4);     p += 4;
        memcpy(p, &s->call_count, 4);       p += 4;
        memcpy(p, &s->fail_count, 4);       p += 4;
        memcpy(p, &s->total_latency_us, 8); p += 8;
        memcpy(p, &s->avg_latency_us, 8);   p += 8;
        memcpy(p, &s->created_at, 8);       p += 8;
        memcpy(p, &s->version, 4);          p += 4;
        uint32_t flags = s->is_sandboxed ? 1 : 0;
        memcpy(p, &flags, 4);               p += 4;
    }

    spin_unlock(&engine->lock);
    return (ssize_t)(p - (uint8_t *)buf);
}

int skill_deserialize(skill_engine_t *engine, const void *buf, size_t size) {
    if (!engine || !buf || size < 12) return -EINVAL;

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t magic;
    memcpy(&magic, p, 4); p += 4;
    if (magic != SKILL_SERIAL_MAGIC) return -EINVAL;

    uint32_t count;
    uint64_t next_id;
    memcpy(&count, p, 4);    p += 4;
    memcpy(&next_id, p, 8);  p += 8;

    engine->next_id = next_id;

    for (uint32_t i = 0; i < count && engine->count < MAX_SKILLS; i++) {
        skill_t *s = &engine->skills[engine->count];
        memset(s, 0, sizeof(*s));

        memcpy(&s->id, p, 8);               p += 8;
        memcpy(s->name, p, 64);             p += 64;
        memcpy(s->description, p, 256);     p += 256;
        memcpy(s->signature, p, 128);       p += 128;
        memcpy(&s->success_rate, p, 4);     p += 4;
        memcpy(&s->call_count, p, 4);       p += 4;
        memcpy(&s->fail_count, p, 4);       p += 4;
        memcpy(&s->total_latency_us, p, 8); p += 8;
        memcpy(&s->avg_latency_us, p, 8);   p += 8;
        memcpy(&s->created_at, p, 8);       p += 8;
        memcpy(&s->version, p, 4);          p += 4;
        uint32_t flags;
        memcpy(&flags, p, 4);               p += 4;
        s->is_sandboxed = (flags & 1) != 0;

        /* fn pointer will need to be re-registered from loaded code */
        s->fn = NULL;
        s->active = true;
        engine->count++;
    }

    return 0;
}
