#pragma once

#include <aevos/types.h>

struct agent;

/* ── L3 Self-Evolution Plane（与 Agent Runtime 并列）──────────────── */

typedef enum {
    SEP_PLAN_REACT = 0,
    SEP_PLAN_TOT   = 1,
} sep_planner_mode_t;

typedef enum {
    SEP_STEP_THOUGHT = 0,
    SEP_STEP_ACTION  = 1,
    SEP_STEP_OBS     = 2,
} sep_step_kind_t;

#define SEP_MAX_PLAN_STEPS 16
#define SEP_STEP_TEXT      128

typedef struct {
    sep_step_kind_t kind;
    char            text[SEP_STEP_TEXT];
} sep_plan_step_t;

typedef struct sep_planner {
    sep_planner_mode_t mode;
    uint32_t           step_count;
    sep_plan_step_t    steps[SEP_MAX_PLAN_STEPS];
    uint32_t           tot_branch;
} sep_planner_t;

typedef struct {
    uint32_t hist_entries;
    uint64_t eventlog_seq;
} sep_corrector_checkpoint_t;

/*
 * 增量验证上下文：简化的 safety CTL（状态机），性质示例
 * 「工具通道不得出现连续两次 [RESULT:FAIL]」。
 */
typedef struct sep_verify_ctx {
    int      ctl_phase; /* 0 初态 1 已见成功 2 已见单次 FAIL */
    uint32_t consecutive_tool_fail;
    bool     last_incremental_ok;
} sep_verify_ctx_t;

typedef struct {
    char     last_ok_task[128];
    uint32_t reuse_hits;
} sep_proof_reuse_t;

typedef struct sep_plane {
    sep_planner_t              planner;
    sep_verify_ctx_t           verify;
    sep_corrector_checkpoint_t ckpt;
    bool                       ckpt_valid;
    sep_proof_reuse_t          proof;
} sep_plane_t;

void sep_plane_init(sep_plane_t *p);
void sep_plane_fini(sep_plane_t *p);

void sep_planner_set_mode(sep_plane_t *p, sep_planner_mode_t mode);
void sep_planner_reset(sep_plane_t *p);
int  sep_planner_push(sep_plane_t *p, sep_step_kind_t kind, const char *text);
void sep_planner_log_to_eventlog(struct agent *agent);

int sep_corrector_checkpoint(struct agent *agent);
int sep_corrector_rollback(struct agent *agent);

void sep_verifier_reset(sep_plane_t *p);
bool sep_verifier_check_history_tail(struct agent *agent, uint32_t max_scan);

int sep_evolver_on_verify_fail(struct agent *agent, const char *hint_task);
