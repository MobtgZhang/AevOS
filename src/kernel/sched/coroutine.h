#pragma once

#include <aevos/types.h>
#include <aevos/config.h>

#define CORO_STACK_SIZE  (64 * 1024)
#define CORO_NAME_MAX    32
#define CORO_PRIORITY_MAX 255
#define CORO_PRIORITY_LEVELS 256

/* L1 协程调度四态（ideas2）：就绪 / 运行 / 阻塞等待 / 已退出待回收 */
typedef enum {
    CORO_READY   = 0,
    CORO_RUNNING = 1,
    CORO_WAITING = 2,
    CORO_DEAD    = 3,
} coro_state_t;

typedef void (*coro_fn_t)(void *arg);

typedef struct coro {
    uint64_t      rsp;
    uint8_t      *stack_base;
    coro_state_t  state;
    uint8_t       priority;
    uint32_t      id;
    uint64_t      wake_tick;
    coro_fn_t     fn;
    void         *arg;
    struct coro  *next;
    char          name[CORO_NAME_MAX];
} coro_t;

extern coro_t *current_coro;

void     coro_init(void);
coro_t  *coro_create(const char *name, coro_fn_t fn, void *arg, uint8_t priority);
void     coro_yield(void);
void     coro_sleep(uint32_t ms);
void     coro_exit(void);
void     scheduler_run(void);

/*
 * 硬件定时器 tick（ISR 上下文）：累计量子，请求 UI 等长协程在安全点 yield。
 */
void     scheduler_kernel_timer_tick(void);

bool     scheduler_preempt_pending(void);

void     scheduler_preempt_clear(void);

/* Architecture-level cancel flag (checked by long-running coroutines). */
void     scheduler_cancel_broadcast_set(bool active);
bool     scheduler_cancel_requested(void);

/* Implemented in arch-specific coro_switch.S */
extern void coro_switch(uint64_t *old_sp, uint64_t new_sp);
extern void coro_entry_trampoline(void);
