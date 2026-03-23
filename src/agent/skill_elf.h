#pragma once

#include "skill.h"

/* Load skill from ELF on VFS path (hotload). Returns -ENOTSUP until dynamic linker exists. */
int skill_load_elf(skill_engine_t *eng, const char *path,
                   const char *name, const char *description);
