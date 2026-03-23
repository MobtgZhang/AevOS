#pragma once

#include <aevos/types.h>

/* NTFS: volume probe + minimal stub (README) until full $MFT driver exists. */
int ntfs_try_mount(const char *mount_path);
