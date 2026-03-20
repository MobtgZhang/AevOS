#include "timer.h"
#include "../arch/io.h"
#include "../klog.h"
#include <aevos/config.h>

/*
 * Intel 8253/8254 PIT (Programmable Interval Timer)
 *
 * Channel 0: system timer (IRQ0)
 * Channel 1: historically DRAM refresh (unused)
 * Channel 2: PC speaker
 */

#define PIT_CHANNEL0_DATA  0x40
#define PIT_CHANNEL2_DATA  0x42
#define PIT_COMMAND        0x43

/* Command byte: channel 0, lo/hi, rate generator */
#define PIT_CMD_CH0_RATE   0x34
#define PIT_BASE_FREQ      1193182UL

static volatile uint64_t tick_count  = 0;
static uint32_t          configured_freq = 0;
static uint64_t          last_check_tick = 0;

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

void timer_handler(void)
{
    tick_count++;
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
