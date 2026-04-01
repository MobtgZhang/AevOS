#include "timer.h"
#include "../arch/io.h"
#include "../klog.h"
#include "../sched/coroutine.h"
#include <aevos/config.h>

static volatile uint64_t tick_count     = 0;
static uint32_t          configured_freq = 0;
static uint64_t          last_check_tick = 0;

#if defined(__x86_64__)

/* ── Intel 8253/8254 PIT (Programmable Interval Timer) ── */

#define PIT_CHANNEL0_DATA  0x40
#define PIT_CHANNEL2_DATA  0x42
#define PIT_COMMAND        0x43
#define PIT_CMD_CH0_RATE   0x34
#define PIT_BASE_FREQ      1193182UL

void timer_init(uint32_t freq_hz)
{
    if (freq_hz == 0) freq_hz = TIMER_FREQ_HZ;
    configured_freq = freq_hz;

    uint32_t divisor = PIT_BASE_FREQ / freq_hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    cli();
    outb(PIT_COMMAND, PIT_CMD_CH0_RATE);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));
    sti();

    tick_count = 0;
    last_check_tick = 0;
    klog("[timer] PIT channel 0 configured at %u Hz (divisor=%u)\n", freq_hz, divisor);
}

#elif defined(__loongarch64)

/*
 * LoongArch Stable Timer (CSR.TCFG / CSR.TVAL / CSR.TICLR)
 *
 * CSR 0x41 (TCFG): Timer Config  — [bit 0]=En, [bit 1]=Periodic, [bits 2+]=InitVal
 * CSR 0x42 (TVAL): Timer Value   — countdown
 * CSR 0x44 (TICLR): Timer Clear  — write bit 0 to clear pending interrupt
 *
 * The stable counter frequency is read from CSR 0x25 (CPUCFG word 4, or CC_FREQ).
 * For QEMU, the stable counter runs at 100MHz typically.
 */

#define CSR_TCFG   0x41
#define CSR_TVAL   0x42
#define CSR_TICLR  0x44
#define CSR_CRMD   0x00
#define CSR_ECFG   0x04

#define LOONGARCH_TIMER_FREQ  100000000UL  /* 100 MHz (QEMU default) */

void timer_init(uint32_t freq_hz)
{
    if (freq_hz == 0) freq_hz = TIMER_FREQ_HZ;
    configured_freq = freq_hz;

    uint64_t period = LOONGARCH_TIMER_FREQ / freq_hz;

    /*
     * TCFG: [bit 0] = Enable, [bit 1] = Periodic, [bits 2..] = InitVal
     * InitVal is shifted left by 2 in the register.
     */
    uint64_t tcfg = (period << 2) | 0x3;  /* enable + periodic */
    __asm__ volatile("csrwr %0, %1" : "+r"(tcfg) : "i"(CSR_TCFG));

    /* Clear any pending timer interrupt */
    uint64_t clear = 1;
    __asm__ volatile("csrwr %0, %1" : "+r"(clear) : "i"(CSR_TICLR));

    /* Enable timer interrupt in ECFG (bit 11 = TI) */
    uint64_t ecfg;
    __asm__ volatile("csrrd %0, %1" : "=r"(ecfg) : "i"(CSR_ECFG));
    ecfg |= (1UL << 11);
    __asm__ volatile("csrwr %0, %1" : "+r"(ecfg) : "i"(CSR_ECFG));

    tick_count = 0;
    last_check_tick = 0;
    klog("[timer] LoongArch stable timer at %u Hz (period=%llu)\n", freq_hz, period);
}

#elif defined(__aarch64__)

/*
 * AArch64 Generic Timer (CNTP_CTL_EL0 / CNTP_TVAL_EL0)
 */

static uint64_t aarch64_timer_tval;

void timer_init(uint32_t freq_hz)
{
    if (freq_hz == 0) freq_hz = TIMER_FREQ_HZ;
    configured_freq = freq_hz;

    uint64_t cntfrq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    if (cntfrq == 0) cntfrq = 62500000UL;

    aarch64_timer_tval = cntfrq / freq_hz;

    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(aarch64_timer_tval));
    uint64_t ctl = 1;   /* ENABLE=1, IMASK=0 */
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(ctl));

    tick_count = 0;
    last_check_tick = 0;
    klog("[timer] aarch64 generic timer at %u Hz (tval=%llu, cntfrq=%llu)\n",
         freq_hz, (unsigned long long)aarch64_timer_tval,
         (unsigned long long)cntfrq);
}

#else /* riscv64 — stub */

void timer_init(uint32_t freq_hz)
{
    if (freq_hz == 0) freq_hz = TIMER_FREQ_HZ;
    configured_freq = freq_hz;
    tick_count = 0;
    last_check_tick = 0;
    klog("[timer] stub timer at %u Hz\n", freq_hz);
}

#endif

/* ── Common API ─────────────────────────────────────── */

void timer_handler(void)
{
#if defined(__loongarch64)
    uint64_t clear = 1;
    __asm__ volatile("csrwr %0, 0x44" : "+r"(clear));
#elif defined(__aarch64__)
    {
        extern uint64_t aarch64_timer_tval;
        __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(aarch64_timer_tval));
    }
#endif
    tick_count++;
    scheduler_kernel_timer_tick();
}

uint64_t timer_get_ticks(void)
{
    return tick_count;
}

uint64_t timer_get_ms(void)
{
    if (configured_freq == 0) return 0;
    return (tick_count * 1000ULL) / configured_freq;
}

void timer_sleep_ms(uint32_t ms)
{
    uint64_t target = tick_count + ((uint64_t)ms * configured_freq) / 1000;
    while (tick_count < target) {
#if defined(__x86_64__)
        __asm__ volatile("hlt");
#elif defined(__aarch64__)
        __asm__ volatile("wfi");
#elif defined(__riscv)
        __asm__ volatile("wfi");
#elif defined(__loongarch64)
        __asm__ volatile("idle 0");
#endif
    }
}

bool tick_elapsed_ms(uint32_t ms)
{
    uint64_t delta_ticks = ((uint64_t)ms * configured_freq) / 1000;
    if (tick_count - last_check_tick >= delta_ticks) {
        last_check_tick = tick_count;
        return true;
    }
    return false;
}
