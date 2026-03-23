#include "sep_plane.h"
#include "agent_core.h"
#include "history.h"
#include "eventlog.h"
#include "evolution.h"
#include "kernel/klog.h"
#include "kernel/mm/slab.h"
#include "lib/string.h"

void sep_plane_init(sep_plane_t *p) {
    if (!p)
        return;
    memset(p, 0, sizeof(*p));
    p->planner.mode = SEP_PLAN_REACT;
    p->verify.last_incremental_ok = true;
}

void sep_plane_fini(sep_plane_t *p) {
    (void)p;
}

void sep_planner_set_mode(sep_plane_t *p, sep_planner_mode_t mode) {
    if (!p)
        return;
    p->planner.mode = mode;
}

void sep_planner_reset(sep_plane_t *p) {
    if (!p)
        return;
    sep_planner_mode_t m = p->planner.mode;
    uint32_t           b = p->planner.tot_branch;
    memset(&p->planner, 0, sizeof(p->planner));
    p->planner.mode       = m;
    p->planner.tot_branch = b;
}

int sep_planner_push(sep_plane_t *p, sep_step_kind_t kind, const char *text) {
    if (!p)
        return -EINVAL;
    if (p->planner.step_count >= SEP_MAX_PLAN_STEPS)
        return -ENOSPC;
    sep_plan_step_t *s = &p->planner.steps[p->planner.step_count++];
    s->kind = kind;
    if (text) {
        strncpy(s->text, text, sizeof(s->text) - 1);
        s->text[sizeof(s->text) - 1] = '\0';
    } else {
        s->text[0] = '\0';
    }
    return 0;
}

void sep_planner_log_to_eventlog(struct agent *agent) {
    if (!agent)
        return;
    sep_plane_t *p = &agent->sep;
    for (uint32_t i = 0; i < p->planner.step_count; i++) {
        char pay[160];
        static const char *tags[] = { "T", "A", "O" };
        const char        *tag = tags[p->planner.steps[i].kind % 3];
        snprintf(pay, sizeof(pay), "%s:%s", tag, p->planner.steps[i].text);
        eventlog_append(EVLOG_PLAN_STEP, agent->id, pay);
    }
}

int sep_corrector_checkpoint(struct agent *agent) {
    if (!agent)
        return -EINVAL;
    agent->sep.ckpt.hist_entries = history_count(&agent->history);
    agent->sep.ckpt.eventlog_seq   = eventlog_seq();
    agent->sep.ckpt_valid          = true;
    return 0;
}

int sep_corrector_rollback(struct agent *agent) {
    if (!agent || !agent->sep.ckpt_valid)
        return -EINVAL;

    uint32_t keep = agent->sep.ckpt.hist_entries;
    int      rc   = history_truncate_keep(&agent->history, keep);
    if (rc != 0)
        return rc;

    char pay[64];
    snprintf(pay, sizeof(pay), "hist=%u evseq=%llu", keep,
             (unsigned long long)agent->sep.ckpt.eventlog_seq);
    eventlog_append(EVLOG_CORRECT_ROLLBACK, agent->id, pay);
    agent->sep.ckpt_valid = false;
    return 0;
}

void sep_verifier_reset(sep_plane_t *p) {
    if (!p)
        return;
    p->verify.ctl_phase            = 0;
    p->verify.consecutive_tool_fail = 0;
    p->verify.last_incremental_ok   = true;
}

static bool line_has_fail(const char *line) {
    return strstr(line, "[RESULT:FAIL]") != NULL || strstr(line, "RESULT:FAIL") != NULL;
}

static bool line_has_ok(const char *line) {
    return strstr(line, "[RESULT:OK]") != NULL;
}

bool sep_verifier_check_history_tail(struct agent *agent, uint32_t max_scan) {
    if (!agent)
        return false;

    sep_plane_t *p = &agent->sep;
    uint32_t     n = history_count(&agent->history);
    if (n == 0) {
        p->verify.last_incremental_ok = true;
        return true;
    }

    uint32_t scan = MIN(max_scan, n);
    char     buf[2048];

    p->verify.last_incremental_ok = true;
    p->verify.consecutive_tool_fail = 0;

    uint32_t      dummy = 0;
    hist_entry_t *meta  = history_get_recent(&agent->history, n, &dummy);

    for (uint32_t k = 0; k < scan; k++) {
        uint32_t idx = n - scan + k;
        ssize_t  len = history_get(&agent->history, idx, buf, sizeof(buf));
        if (len <= 0)
            continue;

        uint8_t role = HIST_ROLE_USER;
        if (meta && idx < dummy)
            role = meta[idx].role;

        if (role != HIST_ROLE_TOOL)
            continue;

        if (line_has_fail(buf)) {
            p->verify.consecutive_tool_fail++;
            if (p->verify.consecutive_tool_fail >= 2) {
                p->verify.last_incremental_ok = false;
                p->verify.ctl_phase         = 2;
            } else {
                p->verify.ctl_phase = 2;
            }
        } else if (line_has_ok(buf)) {
            p->verify.consecutive_tool_fail = 0;
            p->verify.ctl_phase            = 1;
        }
    }

    if (meta)
        kfree(meta);

    char vpay[96];
    snprintf(vpay, sizeof(vpay), "ok=%d fail_streak=%u phase=%d",
             (int)p->verify.last_incremental_ok,
             p->verify.consecutive_tool_fail, p->verify.ctl_phase);
    eventlog_append(EVLOG_VERIFY, agent->id, vpay);

    return p->verify.last_incremental_ok;
}

int sep_evolver_on_verify_fail(struct agent *agent, const char *hint_task) {
    if (!agent)
        return -EINVAL;

    const char *task = hint_task && hint_task[0] ? hint_task : "recover from verify failure";

    if (strncmp(agent->sep.proof.last_ok_task, task, sizeof(agent->sep.proof.last_ok_task) - 1) == 0) {
        agent->sep.proof.reuse_hits++;
        klog("sep_evolver: proof reuse skip (hits=%u)\n", agent->sep.proof.reuse_hits);
        eventlog_append(EVLOG_EVOLVER, agent->id, "reuse_skip");
        return 0;
    }

    if (!agent->evolution.auto_evolve_enabled) {
        eventlog_append(EVLOG_EVOLVER, agent->id, "auto_off");
        return 0;
    }

    int rc = agent_evolve_skill(agent, task);
    if (rc == 0) {
        strncpy(agent->sep.proof.last_ok_task, task,
                sizeof(agent->sep.proof.last_ok_task) - 1);
        eventlog_append(EVLOG_EVOLVER, agent->id, "evolved");
    } else {
        char e[64];
        snprintf(e, sizeof(e), "fail rc=%d", rc);
        eventlog_append(EVLOG_EVOLVER, agent->id, e);
    }
    return rc;
}
