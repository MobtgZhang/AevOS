#include "devfs.h"
#include "vfs.h"
#include <kernel/mm/slab.h>
#include <kernel/klog.h>
#include <lib/string.h>

typedef enum {
    DEV_ROOT = 0,
    DEV_NULL,
    DEV_ZERO,
} dev_kind_t;

typedef struct {
    dev_kind_t kind;
} dev_data_t;

static dev_data_t *dev_resolve(vfs_node_t *node)
{
    if (!node)
        return NULL;
    if (node->fs_data)
        return (dev_data_t *)node->fs_data;
    if (node->mount_data) {
        vfs_node_t *mr = (vfs_node_t *)node->mount_data;
        if (mr && mr->fs_data)
            return (dev_data_t *)mr->fs_data;
    }
    return NULL;
}

static ssize_t devfs_read(vfs_node_t *node, uint64_t offset, void *buf, size_t size)
{
    (void)offset;
    dev_data_t *dd = dev_resolve(node);
    if (!dd || dd->kind == DEV_ROOT)
        return -EISDIR;
    if (dd->kind == DEV_NULL)
        return 0;
    if (dd->kind == DEV_ZERO) {
        memset(buf, 0, size);
        return (ssize_t)size;
    }
    return -EIO;
}

static ssize_t devfs_write(vfs_node_t *node, uint64_t offset, const void *buf, size_t size)
{
    dev_data_t *dd = dev_resolve(node);
    (void)offset;
    (void)buf;
    if (!dd || dd->kind == DEV_ROOT)
        return -EISDIR;
    if (dd->kind == DEV_NULL || dd->kind == DEV_ZERO)
        return (ssize_t)size; /* discard */
    return -EIO;
}

static int devfs_open(vfs_node_t *node, uint32_t flags)
{
    dev_data_t *dd = dev_resolve(node);
    (void)flags;
    if (!dd)
        return -ENOENT;
    return 0;
}

static int devfs_close(vfs_node_t *node)
{
    (void)node;
    return 0;
}

static int devfs_readdir(vfs_node_t *node, vfs_dirent_t *entries, size_t max_entries)
{
    dev_data_t *dd = dev_resolve(node);
    if (!dd || dd->kind != DEV_ROOT)
        return -ENOTDIR;
    static const char *names[] = { "null", "zero" };
    size_t n = sizeof(names) / sizeof(names[0]);
    if (n > max_entries)
        n = max_entries;
    for (size_t i = 0; i < n; i++) {
        strncpy(entries[i].name, names[i], VFS_NAME_MAX - 1);
        entries[i].name[VFS_NAME_MAX - 1] = '\0';
        entries[i].type = VFS_DEVICE;
        entries[i].inode = i + 1;
        entries[i].size = 0;
    }
    return (int)n;
}

static int devfs_stat(vfs_node_t *node, vfs_stat_t *st)
{
    dev_data_t *dd = dev_resolve(node);
    if (!dd)
        return -ENOENT;
    memset(st, 0, sizeof(*st));
    st->type = (dd->kind == DEV_ROOT) ? VFS_DIR : VFS_DEVICE;
    return 0;
}

static vfs_node_t *devfs_lookup(vfs_node_t *parent, const char *name)
{
    dev_data_t *pd = dev_resolve(parent);
    if (!pd || pd->kind != DEV_ROOT)
        return NULL;

    dev_kind_t k = DEV_ROOT;
    if (strcmp(name, "null") == 0)
        k = DEV_NULL;
    else if (strcmp(name, "zero") == 0)
        k = DEV_ZERO;
    else
        return NULL;

    vfs_node_t *ch = (vfs_node_t *)kcalloc(1, sizeof(vfs_node_t));
    dev_data_t *cd = (dev_data_t *)kcalloc(1, sizeof(dev_data_t));
    if (!ch || !cd) {
        kfree(ch);
        kfree(cd);
        return NULL;
    }
    strncpy(ch->name, name, VFS_NAME_MAX - 1);
    ch->type = VFS_DEVICE;
    ch->ops = parent->ops;
    ch->fs_data = cd;
    cd->kind = k;
    return ch;
}

static vfs_ops_t devfs_ops = {
    .read    = devfs_read,
    .write   = devfs_write,
    .open    = devfs_open,
    .close   = devfs_close,
    .readdir = devfs_readdir,
    .mkdir   = NULL,
    .unlink  = NULL,
    .stat    = devfs_stat,
    .lookup  = devfs_lookup,
};

int devfs_init(void)
{
    vfs_node_t *root = (vfs_node_t *)kcalloc(1, sizeof(vfs_node_t));
    dev_data_t *pd = (dev_data_t *)kcalloc(1, sizeof(dev_data_t));
    if (!root || !pd) {
        kfree(root);
        kfree(pd);
        return -ENOMEM;
    }
    strncpy(root->name, "dev", VFS_NAME_MAX - 1);
    root->type = VFS_DIR;
    root->ops = &devfs_ops;
    root->fs_data = pd;
    pd->kind = DEV_ROOT;

    int rc = vfs_mount("/dev", &devfs_ops, root);
    if (rc != 0) {
        kfree(pd);
        kfree(root);
        return rc;
    }
    klog("devfs: mounted at /dev\n");
    return 0;
}
