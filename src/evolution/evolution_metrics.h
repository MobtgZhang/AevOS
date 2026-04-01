#pragma once

#include <aevos/types.h>

/*
 * content1 进化度量 E(t) 的工程采样（简化标量，供 Meta-Agent 读取）。
 */
typedef struct {
    uint64_t crash_events;
    uint64_t syscall_samples;
    uint64_t syscall_latency_ticks_sum;
    uint32_t skill_success;
    uint32_t skill_attempts;
    uint32_t patch_apply_ok;
    uint32_t patch_revert_count;
} evolution_metrics_t;

void evolution_metrics_init(void);

evolution_metrics_t *evolution_metrics_get(void);

void evolution_metrics_note_crash(void);

void evolution_metrics_note_syscall_latency(uint64_t delta_ticks);

void evolution_metrics_note_skill_outcome(bool success);

void evolution_metrics_note_patch(bool applied_ok, bool reverted);

/* 近似 E(t) 的标量合成（任意尺度，仅用于趋势）。 */
float evolution_metrics_score_E(void);
