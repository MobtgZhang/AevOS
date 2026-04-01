#include "linux_abi.h"
#include <aevos/types.h>
#include "../evolution/evolution_metrics.h"

static linux_dispatch_domain_t domain_for_nr(uint32_t nr)
{
    switch (nr) {
    /* 文件与元数据（Docker 常用路径） */
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 13:
    case 14:
    case 17:
    case 72:
    case 78:
    case 79:
    case 89:
    case 97:
    case 102:
    case 131:
    case 217:
    case 257:
    case 262:
    case 263:
    case 267:
    case 269:
        return LINUX_DISPATCH_VFS;
    case 9:
    case 10:
    case 11:
    case 12:
    case 23:
    case 24:
    case 28:
        return LINUX_DISPATCH_MM;
    case 35:
    case 39:
    case 56:
    case 57:
    case 59:
    case 60:
    case 63:
    case 186:
    case 231:
    case 234:
    case 237:
    case 281:
        return LINUX_DISPATCH_PROC;
    case 41:
    case 42:
    case 44:
    case 45:
    case 46:
    case 47:
    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 291:
    case 293:
    case 318:
        return LINUX_DISPATCH_NET;
    case 16:
    case 96:
    case 158:
    case 202:
    case 230:
    case 270:
    case 271:
    case 273:
        return LINUX_DISPATCH_SIGNAL;
    default:
        return LINUX_DISPATCH_ENOSYS;
    }
}

linux_dispatch_domain_t linux_syscall_dispatch_domain(uint32_t linux_nr)
{
    return domain_for_nr(linux_nr);
}

int linux_syscall_dispatch_stub(uint32_t linux_nr, void *opaque)
{
    (void)opaque;
    evolution_metrics_note_syscall_latency(1);
    (void)linux_nr;
    return -ENOTSUP;
}
