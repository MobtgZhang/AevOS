#pragma once

#include <aevos/types.h>

/*
 * Logical 512-byte LBA addressing. Backends (NVMe / AHCI) translate as needed.
 */
bool block_dev_read(uint64_t lba512, uint32_t num_sectors, void *buffer);
bool block_dev_write(uint64_t lba512, uint32_t num_sectors, const void *buffer);
bool block_dev_is_available(void);

/* Prefer NVMe, then AHCI SATA when present. */
void block_storage_register_default(void);
