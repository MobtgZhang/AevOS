#include "linux_abi.h"
#include "kernel/klog.h"

static const struct {
    uint32_t     nr;
    const char  *name;
} g_x64_names[] = {
    { 0,   "read" },
    { 1,   "write" },
    { 2,   "open" },
    { 3,   "close" },
    { 9,   "mmap" },
    { 10,  "mprotect" },
    { 11,  "munmap" },
    { 12,  "brk" },
    { 39,  "getpid" },
    { 56,  "clone" },
    { 57,  "fork" },
    { 59,  "execve" },
    { 60,  "exit" },
    { 63,  "uname" },
    { 89,  "readlink" },
    { 96,  "gettimeofday" },
    { 158, "arch_prctl" },
    { 186, "gettid" },
    { 202, "futex" },
    { 217, "getdents64" },
    { 230, "clock_nanosleep" },
    { 231, "exit_group" },
    { 257, "openat" },
    { 262, "newfstatat" },
    { 263, "unlinkat" },
    { 267, "readlinkat" },
    { 269, "faccessat" },
    { 270, "pselect6" },
    { 271, "ppoll" },
    { 281, "execveat" },
};

const char *linux_abi_x64_syscall_name(uint32_t nr)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_x64_names); i++) {
        if (g_x64_names[i].nr == nr)
            return g_x64_names[i].name;
    }
    return "unknown";
}

/* 供 lc_linux_x64_syscall_name 转发的稳定符号。 */
const char *lc_linux_x64_syscall_name(uint32_t nr)
{
    return linux_abi_x64_syscall_name(nr);
}

void linux_compat_init(void)
{
    klog("linux: x86_64 syscall name table (%u entries)\n",
         (unsigned)ARRAY_SIZE(g_x64_names));
}
