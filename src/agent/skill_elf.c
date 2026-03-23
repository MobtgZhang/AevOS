#include "skill_elf.h"
#include "kernel/klog.h"

int skill_load_elf(skill_engine_t *eng, const char *path,
                   const char *name, const char *description)
{
    (void)eng;
    (void)path;
    (void)name;
    (void)description;
    klog("skill_elf: dynamic ELF load not yet implemented\n");
    return -ENOTSUP;
}
