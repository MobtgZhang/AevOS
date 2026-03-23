#pragma once

/*
 * AevOS POSIX.1 风格子集（内核内实现，映射到 VFS / 定时器）
 *
 * 提供: open/close/read/write/lseek、stat/fstat、mkdir、getpid、sleep、errno。
 * 与内核 KERN 风格返回码并存；使用本头时 I/O 失败请检查 errno 与 P_ENOENT 等常量。
 */

#include <posix/errno.h>
#include <posix/fcntl.h>
#include <posix/sys/types.h>
#include <posix/sys/stat.h>
#include <posix/unistd.h>
