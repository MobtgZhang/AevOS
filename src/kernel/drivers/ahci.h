#pragma once

#include <aevos/types.h>

bool ahci_init(void);
bool ahci_is_ready(void);
bool ahci_read_sectors(uint64_t lba512, uint32_t count, void *buffer);
bool ahci_write_sectors(uint64_t lba512, uint32_t count, const void *buffer);
