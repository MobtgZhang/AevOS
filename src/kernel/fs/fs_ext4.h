#pragma once

#include <aevos/types.h>

/*
 * Read-only EXT4: root directory via extents + small regular files using
 * the first extent leaf (common for tiny files).
 */
int ext4_try_mount(const char *mount_path);
