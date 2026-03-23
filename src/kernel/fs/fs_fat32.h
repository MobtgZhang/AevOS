#pragma once

#include <aevos/types.h>

/* Read-only FAT32 on a block device (512-byte LBAs). */
int fat32_try_mount(const char *mount_path);
