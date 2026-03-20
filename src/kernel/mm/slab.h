#pragma once

#include <aevos/types.h>

void  slab_init(void);

void *kmalloc(size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void *kcalloc(size_t count, size_t size);
