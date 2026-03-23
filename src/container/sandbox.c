#include "lc_layer.h"
#include "kernel/klog.h"

/*
 * Syscall allowlists are Linux x86_64 numbers. Execution inside AevOS remains
 * coroutine-based; this gates future static musl payloads / VM traps.
 */
static const uint32_t g_skill_allow[] = {
    0,   /* read */
    1,   /* write */
    2,   /* open */
    3,   /* close */
    9,   /* mmap */
    10,  /* mprotect */
    11,  /* munmap */
    12,  /* brk */
    60,  /* exit */
    231, /* exit_group */
    UINT32_MAX
};

static const uint32_t g_docker_allow[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 16, 17, 23, 24, 28, 35, 39, 41, 42, 56, 57, 59, 60, 63, 72, 78,
    79, 89, 96, 97, 102, 131, 158, 186, 202, 217, 230, 231, 234, 237, 270,
    273, 281, 291, 293, 318,
    257, /* openat */
    262, /* newfstatat */
    263, /* unlinkat */
    267, /* readlinkat */
    269, /* faccessat */
    270, /* pselect6 */
    271, /* ppoll */
    273, /* set_robust_list */
    281, /* execveat — gated by IFC in real runner */
    UINT32_MAX
};

static bool in_list(const uint32_t *list, uint32_t nr)
{
    for (int i = 0; list[i] != UINT32_MAX; i++) {
        if (list[i] == nr)
            return true;
    }
    return false;
}

void lc_sandbox_init(void)
{
    klog("lc: syscall sandbox profiles (skill=%u syscalls, docker=%u syscalls)\n",
         (unsigned)(ARRAY_SIZE(g_skill_allow) - 1),
         (unsigned)(ARRAY_SIZE(g_docker_allow) - 1));
}

bool lc_sandbox_syscall_allow(lc_sb_profile_t prof, uint32_t linux_nr)
{
    switch (prof) {
    case LC_SB_PROFILE_SKILL:
        return in_list(g_skill_allow, linux_nr);
    case LC_SB_PROFILE_DOCKER:
        return in_list(g_docker_allow, linux_nr);
    default:
        return false;
    }
}

int lc_linux_syscall_gate(lc_sb_profile_t prof, lc_ifc_domain_t from_dom,
                          uint32_t linux_nr)
{
    if (!lc_sandbox_syscall_allow(prof, linux_nr))
        return -1;
    /*
     * Docker-profile payloads talk to the Linux ABI shim backed by host VFS;
     * require container→host I/O clearance. Skill syscalls are emulated inside
     * LC and do not cross this boundary.
     */
    if (prof == LC_SB_PROFILE_DOCKER && from_dom == LC_IFC_DOM_CONTAINER) {
        if (!lc_ifc_flow_allowed(from_dom, LC_IFC_DOM_HOST,
                                 LC_IFC_OP_READ | LC_IFC_OP_WRITE))
            return -1;
    }
    return 0;
}
