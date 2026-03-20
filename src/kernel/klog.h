#pragma once

#include <aevos/types.h>

void klog(const char *fmt, ...);
void NORETURN kpanic(const char *fmt, ...);
