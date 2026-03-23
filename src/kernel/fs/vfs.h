#pragma once

#include <aevos/types.h>

#define VFS_NAME_MAX     128
#define VFS_PATH_MAX     512
#define VFS_O_READ       0x01
#define VFS_O_WRITE      0x02
#define VFS_O_CREATE     0x04
#define VFS_O_TRUNC      0x08
#define VFS_O_APPEND     0x10

typedef int vfs_fd_t;

typedef enum {
    VFS_FILE,
    VFS_DIR,
    VFS_SYMLINK,
    VFS_DEVICE
} vfs_node_type_t;

typedef struct vfs_node    vfs_node_t;
typedef struct vfs_ops     vfs_ops_t;
typedef struct vfs_dirent  vfs_dirent_t;
typedef struct vfs_stat    vfs_stat_t;

struct vfs_dirent {
    char             name[VFS_NAME_MAX];
    vfs_node_type_t  type;
    uint64_t         inode;
    uint64_t         size;
};

struct vfs_stat {
    vfs_node_type_t  type;
    uint64_t         size;
    uint32_t         permissions;
    uint64_t         inode;
    uint64_t         created;
    uint64_t         modified;
};

struct vfs_ops {
    ssize_t (*read)(vfs_node_t *node, uint64_t offset, void *buf, size_t size);
    ssize_t (*write)(vfs_node_t *node, uint64_t offset, const void *buf, size_t size);
    int     (*open)(vfs_node_t *node, uint32_t flags);
    int     (*close)(vfs_node_t *node);
    int     (*readdir)(vfs_node_t *node, vfs_dirent_t *entries, size_t max_entries);
    int     (*mkdir)(vfs_node_t *parent, const char *name);
    int     (*unlink)(vfs_node_t *parent, const char *name);
    int     (*stat)(vfs_node_t *node, vfs_stat_t *st);
    /* Optional: resolve a child name not yet in the in-memory tree (disk fs). */
    vfs_node_t *(*lookup)(vfs_node_t *parent, const char *name);
};

struct vfs_node {
    char             name[VFS_NAME_MAX];
    vfs_node_type_t  type;
    uint64_t         size;
    uint32_t         permissions;
    uint64_t         inode;
    void            *fs_data;
    vfs_ops_t       *ops;
    vfs_node_t      *parent;
    vfs_node_t      *children;
    vfs_node_t      *next_sibling;

    /* mount point overlay */
    vfs_ops_t       *mount_ops;
    void            *mount_data;
};

int           vfs_init(void);
int           vfs_mount(const char *path, vfs_ops_t *fs_ops, void *fs_data);
vfs_fd_t      vfs_open(const char *path, uint32_t flags);
int           vfs_close(vfs_fd_t fd);
ssize_t       vfs_read(vfs_fd_t fd, void *buf, size_t size);
ssize_t       vfs_write(vfs_fd_t fd, const void *buf, size_t size);
int           vfs_mkdir(const char *path);
int           vfs_readdir(vfs_fd_t fd, vfs_dirent_t *entries, size_t max_entries);
int           vfs_stat(const char *path, vfs_stat_t *st);
int           vfs_fstat(vfs_fd_t fd, vfs_stat_t *st);
int64_t       vfs_lseek(vfs_fd_t fd, int64_t offset, int whence);
vfs_node_t   *vfs_resolve_path(const char *path);
vfs_node_t   *vfs_make_child(vfs_node_t *parent, const char *name,
                             vfs_node_type_t type, void *fs_data);

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2
