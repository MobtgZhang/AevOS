#include "eventlog.h"
#include "kernel/drivers/timer.h"
#include "lib/string.h"

static eventlog_record_t g_ring[EVENTLOG_CAPACITY];
static uint32_t          g_head;
static uint32_t          g_count;
static uint64_t          g_seq;
static spinlock_t        g_lock = SPINLOCK_INIT;

void eventlog_init(void)
{
    spin_lock(&g_lock);
    g_head = 0;
    g_count = 0;
    g_seq = 0;
    memset(g_ring, 0, sizeof(g_ring));
    spin_unlock(&g_lock);
}

void eventlog_append(eventlog_type_t type, uint64_t agent_id, const char *payload)
{
    spin_lock(&g_lock);
    uint32_t slot = (g_head + g_count) % EVENTLOG_CAPACITY;
    if (g_count < EVENTLOG_CAPACITY)
        g_count++;
    else
        g_head = (g_head + 1) % EVENTLOG_CAPACITY;

    eventlog_record_t *r = &g_ring[slot];
    r->seq     = ++g_seq;
    r->ts_ms   = timer_get_ms();
    r->type    = type;
    r->agent_id = agent_id;
    if (payload) {
        strncpy(r->payload, payload, sizeof(r->payload) - 1);
        r->payload[sizeof(r->payload) - 1] = '\0';
    } else {
        r->payload[0] = '\0';
    }
    spin_unlock(&g_lock);
}

uint64_t eventlog_seq(void)
{
    spin_lock(&g_lock);
    uint64_t s = g_seq;
    spin_unlock(&g_lock);
    return s;
}

uint32_t eventlog_snapshot(eventlog_record_t *out, uint32_t max_records)
{
    if (!out || max_records == 0)
        return 0;
    spin_lock(&g_lock);
    uint32_t n = g_count < max_records ? g_count : max_records;
    uint32_t start = (g_head + g_count - n) % EVENTLOG_CAPACITY;
    for (uint32_t i = 0; i < n; i++)
        out[i] = g_ring[(start + i) % EVENTLOG_CAPACITY];
    spin_unlock(&g_lock);
    return n;
}
