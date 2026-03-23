#pragma once

/*
 * open() 标志：与常见 glibc / Linux 数值一致（仅本层使用，映射到 VFS_O_*）
 */

#include <posix/sys/types.h>

#define O_ACCMODE   00000003
#define O_RDONLY    00000000
#define O_WRONLY    00000001
#define O_RDWR      00000002
#define O_CREAT     00000100
#define O_EXCL      00000200
#define O_TRUNC     00001000
#define O_APPEND    00002000

#define S_IRWXU     00700
#define S_IRUSR     00400
#define S_IWUSR     00200
#define S_IXUSR     00100
