#pragma once

/*
 * POSIX sys/types.h 子集（AevOS 裸机）
 */

#include <aevos/types.h>

typedef int32_t  pid_t;

typedef int64_t  posix_off_t;
typedef uint32_t posix_mode_t;
typedef uint32_t posix_uid_t;
typedef uint32_t posix_gid_t;
typedef uint64_t posix_dev_t;
typedef uint64_t posix_ino_t;
typedef uint32_t posix_nlink_t;
typedef int64_t  posix_blksize_t;
typedef int64_t  posix_blkcnt_t;
typedef int64_t  posix_time_t;
