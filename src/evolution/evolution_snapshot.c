#include "evolution_snapshot.h"
#include "../kernel/klog.h"
#include "../kernel/drivers/timer.h"
#include "lib/string.h"

#define EV_SNAPSHOT_MAX 8

static evolution_snapshot_meta_t g_snaps[EV_SNAPSHOT_MAX];
static int                       g_snap_count;

void evolution_snapshot_init(void)
{
    memset(g_snaps, 0, sizeof(g_snaps));
    g_snap_count = 0;
    klog("evolution: snapshot engine scaffold (max=%d)\n", EV_SNAPSHOT_MAX);
}

int evolution_snapshot_register_current(const char *label)
{
    if (g_snap_count >= EV_SNAPSHOT_MAX)
        return -ENOSPC;
    evolution_snapshot_meta_t *m = &g_snaps[g_snap_count];
    m->version      = 1;
    m->kernel_elf_crc = 0;
    m->created_tick = timer_get_ticks();
    snprintf(m->label, sizeof(m->label), "%s",
             label && label[0] ? label : "unnamed");
    g_snap_count++;
    klog("evolution: snapshot registered #%d '%s'\n",
         g_snap_count - 1, m->label);
    return g_snap_count - 1;
}

int evolution_snapshot_count(void)
{
    return g_snap_count;
}

int evolution_snapshot_rollback(int snapshot_id)
{
    if (snapshot_id < 0 || snapshot_id >= g_snap_count)
        return -ENOENT;
    klog("evolution: rollback requested → snapshot #%d '%s' (stub)\n",
         snapshot_id, g_snaps[snapshot_id].label);
    return 0;
}
