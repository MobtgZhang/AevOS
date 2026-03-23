#pragma once

/*
 * POSIX.1 unistd.h 子集 — 由 VFS / 定时器实现。
 */

#include <posix/errno.h>
#include <posix/sys/types.h>
#include <posix/fcntl.h>
#include <posix/sys/stat.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int     posix_open(const char *path, int oflag, ...);
int     posix_close(int fd);
ssize_t posix_read(int fd, void *buf, size_t count);
ssize_t posix_write(int fd, const void *buf, size_t count);
posix_off_t posix_lseek(int fd, posix_off_t offset, int whence);

int     posix_stat(const char *path, struct stat *buf);
int     posix_fstat(int fd, struct stat *buf);

int     posix_mkdir(const char *path, posix_mode_t mode);

pid_t   posix_getpid(void);
unsigned int posix_sleep(unsigned int seconds);

void    posix_init(void);
