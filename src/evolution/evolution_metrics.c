#include "evolution_metrics.h"
#include "../kernel/klog.h"
#include "lib/string.h"

static evolution_metrics_t g_m;

void evolution_metrics_init(void)
{
    memset(&g_m, 0, sizeof(g_m));
    klog("evolution: metrics E(t) sampler init\n");
}

evolution_metrics_t *evolution_metrics_get(void)
{
    return &g_m;
}

void evolution_metrics_note_crash(void)
{
    g_m.crash_events++;
}

void evolution_metrics_note_syscall_latency(uint64_t delta_ticks)
{
    g_m.syscall_samples++;
    g_m.syscall_latency_ticks_sum += delta_ticks;
}

void evolution_metrics_note_skill_outcome(bool success)
{
    g_m.skill_attempts++;
    if (success)
        g_m.skill_success++;
}

void evolution_metrics_note_patch(bool applied_ok, bool reverted)
{
    if (applied_ok)
        g_m.patch_apply_ok++;
    if (reverted)
        g_m.patch_revert_count++;
}

float evolution_metrics_score_E(void)
{
    /* α·Stability + β·Perf + γ·Coverage − δ·RevertRate （系数取 1 占位） */
    float stability = g_m.crash_events == 0 ? 1.0f
                                          : 1.0f / (float)(1u + g_m.crash_events);
    float perf    = 1.0f;
    if (g_m.syscall_samples > 0) {
        float avg = (float)g_m.syscall_latency_ticks_sum
                    / (float)g_m.syscall_samples;
        perf = 1.0f / (1.0f + avg / 1000.0f);
    }
    float coverage = g_m.skill_attempts == 0 ? 0.0f
        : (float)g_m.skill_success / (float)g_m.skill_attempts;
    float revert_pen = g_m.patch_apply_ok == 0 ? 0.0f
        : (float)g_m.patch_revert_count / (float)(1u + g_m.patch_apply_ok);
    return stability + perf + coverage - revert_pen;
}
