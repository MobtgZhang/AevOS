#include "fs_ntfs.h"
#include "vfs.h"
#include <kernel/drivers/block_dev.h>
#include <kernel/mm/slab.h>
#include <kernel/klog.h>
#include <lib/string.h>

typedef struct {
    uint32_t part_lba;
} ntfs_ctx_t;

typedef struct {
    uint8_t is_readme;
} ntfs_node_t;

static const char ntfs_readme_body[] =
    "AevOS NTFS: stub volume driver.\r\n"
    "Full NTFS requires $MFT, attribute lists, index trees, and security "
    "descriptors.\r\n"
    "Use FAT32 or EXT4 mounts for real file access on the same block device.\r\n";

static ntfs_ctx_t *ctx_of(const vfs_node_t *n)
{
    const vfs_node_t *p = n;
    while (p) {
        if (p->mount_data)
            return (ntfs_ctx_t *)p->mount_data;
        p = p->parent;
    }
    return NULL;
}

static int ntfs_stat(vfs_node_t *node, vfs_stat_t *st)
{
    ntfs_node_t *fn = (ntfs_node_t *)node->fs_data;
    if (!fn)
        return -EINVAL;
    if (fn->is_readme) {
        st->type        = VFS_FILE;
        st->size        = sizeof(ntfs_readme_body) - 1;
        st->permissions = 0444;
        st->inode       = 1;
        st->created     = 0;
        st->modified    = 0;
        return 0;
    }
    st->type        = VFS_DIR;
    st->size        = 0;
    st->permissions = 0555;
    st->inode       = 2;
    st->created     = 0;
    st->modified    = 0;
    return 0;
}

static ssize_t ntfs_read(vfs_node_t *node, uint64_t offset, void *buf, size_t size)
{
    ntfs_node_t *fn = (ntfs_node_t *)node->fs_data;
    if (!fn || !fn->is_readme)
        return -EINVAL;
    size_t total = sizeof(ntfs_readme_body) - 1;
    if (offset >= total)
        return 0;
    size_t rem = total - (size_t)offset;
    if (size > rem)
        size = rem;
    memcpy(buf, ntfs_readme_body + offset, size);
    return (ssize_t)size;
}

static int ntfs_readdir(vfs_node_t *node, vfs_dirent_t *entries, size_t max)
{
    ntfs_node_t *fn = (ntfs_node_t *)node->fs_data;
    if (!fn || fn->is_readme)
        return -EINVAL;
    if (max == 0)
        return 0;
    strncpy(entries[0].name, "NTFS_README.TXT", VFS_NAME_MAX - 1);
    entries[0].name[VFS_NAME_MAX - 1] = '\0';
    entries[0].type  = VFS_FILE;
    entries[0].inode = 1;
    entries[0].size  = sizeof(ntfs_readme_body) - 1;
    return 1;
}

static vfs_node_t *ntfs_lookup(vfs_node_t *parent, const char *name)
{
    ntfs_ctx_t *c = ctx_of(parent);
    ntfs_node_t *pd = (ntfs_node_t *)parent->fs_data;
    if (!c || !pd || pd->is_readme)
        return NULL;
    if (strcmp(name, "NTFS_README.TXT") != 0)
        return NULL;
    ntfs_node_t *ch = (ntfs_node_t *)kmalloc(sizeof(ntfs_node_t));
    if (!ch)
        return NULL;
    ch->is_readme = 1;
    return vfs_make_child(parent, name, VFS_FILE, ch);
}

static vfs_ops_t ntfs_ops = {
    .read    = ntfs_read,
    .write   = NULL,
    .open    = NULL,
    .close   = NULL,
    .readdir = ntfs_readdir,
    .mkdir   = NULL,
    .unlink  = NULL,
    .stat    = ntfs_stat,
    .lookup  = ntfs_lookup,
};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool boot_is_ntfs(const uint8_t *s)
{
    return memcmp(s + 3, "NTFS    ", 8) == 0;
}

static bool probe_ntfs(ntfs_ctx_t *out)
{
    uint8_t sec[512];
    if (!block_dev_read(0, 1, sec))
        return false;
    if (boot_is_ntfs(sec)) {
        out->part_lba = 0;
        return true;
    }
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return false;
    for (int i = 0; i < 4; i++) {
        uint8_t *pe = sec + 0x1BE + i * 16;
        uint8_t typ = pe[4];
        if (typ != 0x07)
            continue;
        uint32_t lba = le32(pe + 8);
        if (!block_dev_read(lba, 1, sec))
            continue;
        if (boot_is_ntfs(sec)) {
            out->part_lba = lba;
            return true;
        }
    }
    return false;
}

int ntfs_try_mount(const char *mount_path)
{
    if (!block_dev_is_available())
        return -EIO;
    ntfs_ctx_t *ctx = (ntfs_ctx_t *)kmalloc(sizeof(ntfs_ctx_t));
    if (!ctx)
        return -ENOMEM;
    memset(ctx, 0, sizeof(*ctx));
    if (!probe_ntfs(ctx)) {
        kfree(ctx);
        return -EINVAL;
    }
    int rc = vfs_mount(mount_path, &ntfs_ops, ctx);
    if (rc < 0) {
        kfree(ctx);
        return rc;
    }
    vfs_node_t *mp = vfs_resolve_path(mount_path);
    if (!mp) {
        kfree(ctx);
        return -ENOENT;
    }
    ntfs_node_t *root = (ntfs_node_t *)kmalloc(sizeof(ntfs_node_t));
    if (!root) {
        kfree(ctx);
        return -ENOMEM;
    }
    root->is_readme = 0;
    mp->fs_data     = root;
    klog("ntfs: stub mounted at %s (partition LBA %u)\n", mount_path, ctx->part_lba);
    return 0;
}
