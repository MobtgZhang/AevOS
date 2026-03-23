#include "history_wal.h"
#include "kernel/drivers/timer.h"
#include "lib/string.h"

void history_wal_init(history_wal_t *w)
{
    if (!w)
        return;
    memset(w, 0, sizeof(*w));
    w->lock = SPINLOCK_INIT;
}

int history_wal_append(history_wal_t *w, uint8_t role,
                       const void *data, uint32_t len)
{
    if (!w || (!data && len > 0))
        return -EINVAL;
    (void)role;
    spin_lock(&w->lock);
    w->base_seq++;
    /* Persistent media hook: write {seq, ts, role, len, payload} via VFS */
    (void)data;
    (void)len;
    (void)timer_get_ms();
    spin_unlock(&w->lock);
    return 0;
}
