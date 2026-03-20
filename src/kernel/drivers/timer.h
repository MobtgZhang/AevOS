#pragma once

#include <aevos/types.h>

void     timer_init(uint32_t freq_hz);
void     timer_handler(void);
uint64_t timer_get_ticks(void);
uint64_t timer_get_ms(void);
void     timer_sleep_ms(uint32_t ms);
bool     tick_elapsed_ms(uint32_t ms);
