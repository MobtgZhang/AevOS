#pragma once

#include <aevos/types.h>

void serial_init(uint16_t port);
void serial_putchar(char c);
void serial_puts(const char *s);
int  serial_read(void);
void serial_write(const void *buf, size_t len);
