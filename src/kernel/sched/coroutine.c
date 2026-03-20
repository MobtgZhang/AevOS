#include "coroutine.h"
#include "../arch/arch.h"
#include "../klog.h"
#include "../drivers/timer.h"
#include "../mm/slab.h"

coro_t *current_coro = NULL;

static coro_t *run_queues[CORO_PRIORITY_LEVELS];
static coro_t  idle_coro;
static coro_t  coro_pool[MAX_COROUTINES];
static bool    coro_used[MAX_COROUTINES];
static uint32_t next_coro_id = 0;

/* ── internal helpers ── */

static void enqueue(coro_t *c)
{
    c->next = NULL;
    coro_t **tail = &run_queues[c->priority];
    while (*tail)
        tail = &(*tail)->next;
    *tail = c;
}

static coro_t *pick_next_coro(void)
{
    for (int p = CORO_PRIORITY_MAX; p >= 0; p--) {
        if (run_queues[p]) {
            coro_t *c = run_queues[p];
            run_queues[p] = c->next;
            c->next = NULL;
            return c;
        }
    }
    return &idle_coro;
}

static coro_t *alloc_coro(void)
{
    for (uint32_t i = 0; i < MAX_COROUTINES; i++) {
        if (!coro_used[i]) {
            coro_used[i] = true;
            coro_t *c = &coro_pool[i];
            c->id = next_coro_id++;
            return c;
        }
    }
    return NULL;
}

static void free_coro(coro_t *c)
{
    uint32_t idx = (uint32_t)(c - coro_pool);
    if (idx < MAX_COROUTINES) {
        if (c->stack_base) {
            kfree(c->stack_base);
            c->stack_base = NULL;
        }
        coro_used[idx] = false;
    }
}

static void wake_sleeping(void)
{
    uint64_t now = timer_get_ticks();
    for (uint32_t i = 0; i < MAX_COROUTINES; i++) {
        if (coro_used[i] && coro_pool[i].state == CORO_WAITING) {
            if (coro_pool[i].wake_tick && now >= coro_pool[i].wake_tick) {
                coro_pool[i].state = CORO_READY;
                coro_pool[i].wake_tick = 0;
                enqueue(&coro_pool[i]);
            }
        }
    }
}

/* ── idle coroutine body ── */

static void idle_fn(void *arg UNUSED)
{
    for (;;)
        arch_idle();
}

/* ── string helpers ── */

static void coro_strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ── public API ── */

void coro_init(void)
{
    for (int i = 0; i < CORO_PRIORITY_LEVELS; i++)
        run_queues[i] = NULL;
    for (uint32_t i = 0; i < MAX_COROUTINES; i++)
        coro_used[i] = false;

    /* Idle coroutine uses current kernel stack; never freed */
    idle_coro.state = CORO_RUNNING;
    idle_coro.priority = 0;
    idle_coro.id = next_coro_id++;
    idle_coro.fn = idle_fn;
    idle_coro.stack_base = NULL;
    idle_coro.wake_tick = 0;
    idle_coro.next = NULL;
    coro_strncpy(idle_coro.name, "idle", CORO_NAME_MAX);

    current_coro = &idle_coro;
    klog("[sched] coroutine scheduler initialized\n");
}

coro_t *coro_create(const char *name, coro_fn_t fn, void *arg, uint8_t priority)
{
    coro_t *c = alloc_coro();
    if (!c) {
        klog("[sched] coro_create: pool exhausted\n");
        return NULL;
    }

    c->stack_base = (uint8_t *)kmalloc(CORO_STACK_SIZE);
    if (!c->stack_base) {
        klog("[sched] coro_create: stack alloc failed\n");
        free_coro(c);
        return NULL;
    }

    c->fn       = fn;
    c->arg      = arg;
    c->priority = priority;
    c->state    = CORO_READY;
    c->wake_tick = 0;
    c->next     = NULL;
    coro_strncpy(c->name, name ? name : "coro", CORO_NAME_MAX);

    uint64_t *sp = (uint64_t *)(c->stack_base + CORO_STACK_SIZE);
    sp = (uint64_t *)((uint64_t)sp & ~0xFULL);

#if defined(__x86_64__)
    *(--sp) = (uint64_t)arg;
    *(--sp) = (uint64_t)fn;
    *(--sp) = (uint64_t)coro_entry_trampoline;
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* rbp */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r15 */
#elif defined(__aarch64__)
    /* trampoline: ldp x9,x0,[sp],#16 → x9=fn, x0=arg */
    *(--sp) = (uint64_t)arg;
    *(--sp) = (uint64_t)fn;
    /* coro_switch restores: x29/x30 first, then x27/x28..x19/x20, then ret(x30) */
    *(--sp) = 0; /* x20 */
    *(--sp) = 0; /* x19 */
    *(--sp) = 0; /* x22 */
    *(--sp) = 0; /* x21 */
    *(--sp) = 0; /* x24 */
    *(--sp) = 0; /* x23 */
    *(--sp) = 0; /* x26 */
    *(--sp) = 0; /* x25 */
    *(--sp) = 0; /* x28 */
    *(--sp) = 0; /* x27 */
    *(--sp) = (uint64_t)coro_entry_trampoline; /* x30/lr */
    *(--sp) = 0; /* x29/fp */
#elif defined(__riscv)
    /* trampoline: ld a5,0(sp); ld a0,8(sp); addi sp,sp,16 */
    *(--sp) = (uint64_t)arg;
    *(--sp) = (uint64_t)fn;
    /* save area: 13 regs (ra, s0-s11) at offsets 0..96 */
    sp -= 13;
    sp[0] = (uint64_t)coro_entry_trampoline; /* ra */
    for (int i = 1; i <= 12; i++) sp[i] = 0; /* s0-s11 */
#elif defined(__loongarch64)
    /* trampoline: ld.d $t0,sp,0; ld.d $a0,sp,8; addi.d sp,sp,16 */
    *(--sp) = (uint64_t)arg;
    *(--sp) = (uint64_t)fn;
    /* save area: 11 regs (ra, fp, s0-s8) at offsets 0..80 */
    sp -= 11;
    sp[0] = (uint64_t)coro_entry_trampoline; /* ra */
    for (int i = 1; i <= 10; i++) sp[i] = 0; /* fp, s0-s8 */
#endif

    c->rsp = (uint64_t)sp;

    enqueue(c);
    klog("[sched] created coro '%s' (id=%u, prio=%u)\n", c->name, c->id, c->priority);
    return c;
}

void coro_yield(void)
{
    coro_t *prev = current_coro;
    if (prev->state == CORO_RUNNING)
        prev->state = CORO_READY;

    wake_sleeping();

    if (prev->state == CORO_READY)
        enqueue(prev);

    coro_t *next = pick_next_coro();
    if (next == prev) {
        prev->state = CORO_RUNNING;
        return;
    }

    next->state = CORO_RUNNING;
    current_coro = next;
    coro_switch(&prev->rsp, next->rsp);
}

void coro_sleep(uint32_t ms)
{
    current_coro->state = CORO_WAITING;
    current_coro->wake_tick = timer_get_ticks() + (uint64_t)ms * TIMER_FREQ_HZ / 1000;
    coro_yield();
}

void coro_exit(void)
{
    current_coro->state = CORO_DEAD;
    klog("[sched] coro '%s' (id=%u) exited\n", current_coro->name, current_coro->id);
    coro_yield();
    for (;;) arch_idle();
}

void scheduler_run(void)
{
    klog("[sched] scheduler running\n");
    for (;;) {
        wake_sleeping();

        /* Reclaim dead coroutines */
        for (uint32_t i = 0; i < MAX_COROUTINES; i++) {
            if (coro_used[i] && coro_pool[i].state == CORO_DEAD)
                free_coro(&coro_pool[i]);
        }

        coro_t *next = pick_next_coro();
        if (next == &idle_coro && current_coro == &idle_coro) {
            arch_idle();
            continue;
        }

        coro_t *prev = current_coro;
        if (prev->state == CORO_RUNNING)
            prev->state = CORO_READY;
        if (prev->state == CORO_READY && prev != &idle_coro)
            enqueue(prev);

        next->state = CORO_RUNNING;
        current_coro = next;
        if (next != prev)
            coro_switch(&prev->rsp, next->rsp);
    }
}
