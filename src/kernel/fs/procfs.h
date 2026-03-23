#pragma once

/* Mount virtual /proc (kernel introspection). Call after vfs_init(). */
int procfs_init(void);
