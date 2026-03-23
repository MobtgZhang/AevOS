#pragma once

/*
 * IEEE Std 1003.1 (POSIX.1) style errno for AevOS user-facing I/O.
 *
 * 内核内部仍使用 <aevos/types.h> 中的 KERN 风格负数返回码（数值与 POSIX 不同）。
 * 在此头文件中，在定义了 AEVOS_KERNEL 的翻译单元内仅提供 P_* 常量，避免与内核宏重名。
 * 比较时请使用: if (errno == P_ENOENT) ...
 */

int *posix_errno_location(void);
#define errno (*posix_errno_location())

/* 将内核 VFS/子系统返回的负数错误码转为 POSIX errno 并写入 errno */
void posix_seterr_from_kerr(int kret);

#define P_EPERM        1
#define P_ENOENT       2
#define P_EIO          5
#define P_EBADF        9
#define P_ENOMEM      12
#define P_EACCES      13
#define P_EBUSY       16
#define P_EEXIST      17
#define P_EINVAL      22
#define P_EMFILE      24
#define P_ENOSPC      28
#define P_ESPIPE      29
#define P_EOPNOTSUPP  95

#ifndef AEVOS_KERNEL
#define EPERM        P_EPERM
#define ENOENT       P_ENOENT
#define EIO          P_EIO
#define EBADF        P_EBADF
#define ENOMEM       P_ENOMEM
#define EACCES       P_EACCES
#define EBUSY        P_EBUSY
#define EEXIST       P_EEXIST
#define EINVAL       P_EINVAL
#define EMFILE       P_EMFILE
#define ENOSPC       P_ENOSPC
#define ESPIPE       P_ESPIPE
#define EOPNOTSUPP   P_EOPNOTSUPP
#endif
