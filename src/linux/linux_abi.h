#pragma once

#include <aevos/types.h>

/*
 * Linux 兼容层 ABI 元数据（类 FreeBSD linuxulator）。
 * 系统调用分发表见 linux_syscall_dispatch.c；x86_64 名称表见 linux_syscall_x86_64.c。
 */

typedef enum {
    LINUX_ABI_UNKNOWN = 0,
    LINUX_ABI_X86_64,
    LINUX_ABI_AARCH64,
    LINUX_ABI_RISCV64,
} linux_abi_arch_t;

typedef enum {
    LINUX_DISPATCH_VFS    = 1,
    LINUX_DISPATCH_MM     = 2,
    LINUX_DISPATCH_PROC   = 3,
    LINUX_DISPATCH_NET    = 4,
    LINUX_DISPATCH_SIGNAL = 5,
    LINUX_DISPATCH_STUB   = 6,
    LINUX_DISPATCH_ENOSYS = 7,
} linux_dispatch_domain_t;

typedef struct {
    uint32_t                 nr;
    linux_dispatch_domain_t  domain;
    const char                *name;
} linux_syscall_desc_t;

void linux_compat_init(void);

const char *linux_abi_x64_syscall_name(uint32_t nr);

/*
 * 路由 Linux 系统调用号到域（OCI / 沙箱调试用）；未覆盖返回 LINUX_DISPATCH_ENOSYS。
 */
linux_dispatch_domain_t linux_syscall_dispatch_domain(uint32_t linux_nr);

/* 与 VFS/POSIX 联调占位：返回 -ENOSYS 直至服务化完成。 */
int linux_syscall_dispatch_stub(uint32_t linux_nr, void *opaque);
