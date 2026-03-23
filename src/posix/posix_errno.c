#include <aevos/types.h>

static int posix_errno_storage;

int *posix_errno_location(void)
{
    return &posix_errno_storage;
}

static int kerr_to_posix(int k)
{
    switch (k) {
    case EPERM:       return 1;
    case ENOENT:      return 2;
    case EIO:         return 5;
    case EBADF:       return 9;
    case ENOMEM:      return 12;
    case EBUSY:       return 16;
    case EEXIST:      return 17;
    case EINVAL:      return 22;
    case ENOSPC:      return 28;
    case ENOTSUP:     return 95;
    case ESKILL_FAIL: return 5;
    default:          return 5;
    }
}

void posix_seterr_from_kerr(int kret)
{
    if (kret >= 0) {
        posix_errno_storage = 0;
        return;
    }
    posix_errno_storage = kerr_to_posix(-kret);
}

void posix_init(void)
{
    posix_errno_storage = 0;
}
