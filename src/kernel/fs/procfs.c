#include "procfs.h"
#include "vfs.h"
#include <aevos/version.h>
#include <kernel/mm/slab.h>
#include <kernel/klog.h>
#include <lib/string.h>

typedef enum {
    PROC_KIND_ROOT = 0,
    PROC_KIND_VERSION,
    PROC_KIND_MEMINFO,
    PROC_KIND_UPTIME,
} proc_kind_t;

typedef struct {
    proc_kind_t kind;
    char        static_buf[512];
    size_t      buf_len;
} proc_data_t;

/* Mount point node has mount_data → synthetic root with PROC_ROOT */
static proc_data_t *proc_resolve_pd(vfs_node_t *node)
{
    if (!node)
        return NULL;
    if (node->fs_data)
        return (proc_data_t *)node->fs_data;
    if (node->mount_data) {
        vfs_node_t *mr = (vfs_node_t *)node->mount_data;
        if (mr && mr->fs_data)
            return (proc_data_t *)mr->fs_data;
    }
    return NULL;
}

static ssize_t procfs_read(vfs_node_t *node, uint64_t offset, void *buf, size_t size)
{
    proc_data_t *pd = proc_resolve_pd(node);
    if (!pd || pd->kind == PROC_KIND_ROOT)
        return -EISDIR;

    if (offset >= pd->buf_len)
        return 0;
    size_t n = pd->buf_len - (size_t)offset;
    if (n > size)
        n = size;
    memcpy(buf, pd->static_buf + offset, n);
    return (ssize_t)n;
}

static ssize_t procfs_write(vfs_node_t *node, uint64_t offset, const void *buf, size_t size)
{
    (void)node;
    (void)offset;
    (void)buf;
    (void)size;
    return -EROFS;
}

static int procfs_open(vfs_node_t *node, uint32_t flags)
{
    proc_data_t *pd = proc_resolve_pd(node);
    if (!pd)
        return -ENOENT;
    if (pd->kind == PROC_KIND_ROOT)
        return (flags & VFS_O_WRITE) ? -EISDIR : 0;
    if (flags & VFS_O_WRITE)
        return -EROFS;
    return 0;
}

static int procfs_close(vfs_node_t *node)
{
    (void)node;
    return 0;
}

static int procfs_readdir(vfs_node_t *node, vfs_dirent_t *entries, size_t max_entries)
{
    proc_data_t *pd = proc_resolve_pd(node);
    if (!pd || pd->kind != PROC_KIND_ROOT)
        return -ENOTDIR;

    static const char *names[] = { "version", "meminfo", "uptime" };
    size_t n = sizeof(names) / sizeof(names[0]);
    if (n > max_entries)
        n = max_entries;
    for (size_t i = 0; i < n; i++) {
        strncpy(entries[i].name, names[i], VFS_NAME_MAX - 1);
        entries[i].name[VFS_NAME_MAX - 1] = '\0';
        entries[i].type = VFS_FILE;
        entries[i].inode = (uint64_t)(i + 1);
        entries[i].size = 0;
    }
    return (int)n;
}

static int procfs_stat(vfs_node_t *node, vfs_stat_t *st)
{
    proc_data_t *pd = proc_resolve_pd(node);
    if (!pd)
        return -ENOENT;
    memset(st, 0, sizeof(*st));
    st->type = (pd->kind == PROC_KIND_ROOT) ? VFS_DIR : VFS_FILE;
    st->size = (pd->kind == PROC_KIND_ROOT) ? 0 : pd->buf_len;
    st->inode = (uint64_t)pd->kind;
    return 0;
}

static vfs_node_t *procfs_lookup(vfs_node_t *parent, const char *name)
{
    proc_data_t *ppd = proc_resolve_pd(parent);
    if (!ppd || ppd->kind != PROC_KIND_ROOT)
        return NULL;

    proc_kind_t kind = PROC_KIND_ROOT;
    if (strcmp(name, "version") == 0)
        kind = PROC_KIND_VERSION;
    else if (strcmp(name, "meminfo") == 0)
        kind = PROC_KIND_MEMINFO;
    else if (strcmp(name, "uptime") == 0)
        kind = PROC_KIND_UPTIME;
    else
        return NULL;

    vfs_node_t *ch = (vfs_node_t *)kcalloc(1, sizeof(vfs_node_t));
    proc_data_t *pd = (proc_data_t *)kcalloc(1, sizeof(proc_data_t));
    if (!ch || !pd) {
        kfree(ch);
        kfree(pd);
        return NULL;
    }

    strncpy(ch->name, name, VFS_NAME_MAX - 1);
    ch->type = VFS_FILE;
    ch->fs_data = pd;
    ch->ops = parent->ops;
    pd->kind = kind;

    switch (kind) {
    case PROC_KIND_VERSION:
        pd->buf_len = (size_t)snprintf(pd->static_buf, sizeof(pd->static_buf),
            "AevOS %s\n", AEVOS_VERSION_STRING);
        break;
    case PROC_KIND_MEMINFO:
        pd->buf_len = (size_t)snprintf(pd->static_buf, sizeof(pd->static_buf),
            "MemTotal:        (see PMM)\nMemFree:         (dynamic)\n");
        break;
    case PROC_KIND_UPTIME:
        pd->buf_len = (size_t)snprintf(pd->static_buf, sizeof(pd->static_buf),
            "uptime: kernel ticks via timer\n");
        break;
    default:
        break;
    }

    return ch;
}

static vfs_ops_t procfs_ops = {
    .read    = procfs_read,
    .write   = procfs_write,
    .open    = procfs_open,
    .close   = procfs_close,
    .readdir = procfs_readdir,
    .mkdir   = NULL,
    .unlink  = NULL,
    .stat    = procfs_stat,
    .lookup  = procfs_lookup,
};

int procfs_init(void)
{
    vfs_node_t *root = (vfs_node_t *)kcalloc(1, sizeof(vfs_node_t));
    proc_data_t *pd = (proc_data_t *)kcalloc(1, sizeof(proc_data_t));
    if (!root || !pd) {
        kfree(root);
        kfree(pd);
        return -ENOMEM;
    }
    strncpy(root->name, "proc", VFS_NAME_MAX - 1);
    root->type = VFS_DIR;
    root->ops = &procfs_ops;
    root->fs_data = pd;
    pd->kind = PROC_KIND_ROOT;

    int rc = vfs_mount("/proc", &procfs_ops, root);
    if (rc != 0) {
        kfree(pd);
        kfree(root);
        return rc;
    }
    klog("procfs: mounted at /proc\n");
    return 0;
}
