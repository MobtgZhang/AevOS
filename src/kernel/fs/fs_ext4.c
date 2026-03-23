#include "fs_ext4.h"
#include "vfs.h"
#include <kernel/drivers/block_dev.h>
#include <kernel/mm/slab.h>
#include <kernel/klog.h>
#include <lib/string.h>

#define EXT4_SUPER_MAGIC   0xEF53
#define EXT4_EXTENTS_FL    0x00080000u
#define EXT4_FT_DIR        2
#define EXT4_FT_REG_FILE   1
#define EXT4_EXT_MAGIC     0xF30A

typedef struct {
    uint32_t part_lba;
    uint32_t block_size;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
} ext4_ctx_t;

typedef struct {
    uint32_t inode;
    uint64_t size;
    uint8_t  is_dir;
    uint32_t data_block0;
    uint32_t data_blocks;
    uint32_t ee_len;
} ext4_node_t;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static ext4_ctx_t *ctx_of(const vfs_node_t *n)
{
    const vfs_node_t *p = n;
    while (p) {
        if (p->mount_data)
            return (ext4_ctx_t *)p->mount_data;
        p = p->parent;
    }
    return NULL;
}

static bool read_bytes(uint64_t byte_off, void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    while (len > 0) {
        uint64_t lba   = byte_off / 512u;
        uint32_t o     = (uint32_t)(byte_off % 512u);
        uint8_t  sec[512];
        if (!block_dev_read(lba, 1, sec))
            return false;
        size_t take = 512u - (size_t)o;
        if (take > len)
            take = len;
        memcpy(out, sec + o, take);
        out += take;
        byte_off += take;
        len -= take;
    }
    return true;
}

static bool read_super(ext4_ctx_t *c, uint8_t *sb1024)
{
    uint64_t base = (uint64_t)c->part_lba * 512u + 1024u;
    return read_bytes(base, sb1024, 1024);
}

static bool parse_super(uint8_t *sb, ext4_ctx_t *c)
{
    if (le16(sb + 56) != EXT4_SUPER_MAGIC)
        return false;
    uint32_t log_bs = le32(sb + 24);
    c->block_size       = 1024u << log_bs;
    c->blocks_per_group = le32(sb + 32);
    c->inodes_per_group = le32(sb + 40);
    c->inode_size       = le32(sb + 88);
    if (c->inode_size < 128 || c->inode_size > 512)
        c->inode_size = 256;
    c->first_data_block = le32(sb + 20);
    return c->block_size >= 1024u;
}

static bool read_inode_raw(ext4_ctx_t *c, uint32_t ino, uint8_t *buf)
{
    uint32_t idx    = (ino > 0) ? ino - 1u : 0u;
    uint32_t g      = idx / c->inodes_per_group;
    uint32_t idx_in = idx % c->inodes_per_group;
    uint64_t gd_off = (uint64_t)c->part_lba * 512u +
                      (uint64_t)(c->first_data_block + 1u) * c->block_size +
                      (uint64_t)g * 32u;
    uint8_t gd[32];
    if (!read_bytes(gd_off, gd, 32))
        return false;
    uint32_t itable0 = le32(gd + 8);
    uint64_t ino_off = (uint64_t)c->part_lba * 512u +
                       (uint64_t)itable0 * c->block_size +
                       (uint64_t)idx_in * c->inode_size;
    return read_bytes(ino_off, buf, c->inode_size);
}

static bool extent_root(const uint8_t *inode, uint32_t *phys_block, uint32_t *n_blocks,
                        uint64_t * isize)
{
    uint32_t flags = le32(inode + 0x20);
    *isize         = le32(inode + 0x04);
    if ((flags & EXT4_EXTENTS_FL) == 0)
        return false;
    const uint8_t *eb = inode + 0x28;
    if (le16(eb) != EXT4_EXT_MAGIC)
        return false;
    uint16_t entries = le16(eb + 2);
    uint16_t depth   = le16(eb + 4);
    if (depth != 0 || entries < 1)
        return false;
    const uint8_t *ex = eb + 12;
    /* ext4_extent */
    *n_blocks    = le16(ex + 4);
    uint16_t hi  = le16(ex + 6);
    uint32_t lo  = le32(ex + 8);
    *phys_block  = lo | ((uint32_t)((uint32_t)hi & 0xFFFFu) << 16);
    return *n_blocks > 0;
}

static bool parse_dir_block(ext4_ctx_t *c, uint32_t block, const char *want,
                            uint32_t *out_ino, ext4_node_t *out_node)
{
    size_t   sz = c->block_size;
    uint8_t *b  = (uint8_t *)kmalloc(sz);
    if (!b)
        return false;
    uint64_t off = (uint64_t)c->part_lba * 512u + (uint64_t)block * c->block_size;
    bool ok = read_bytes(off, b, sz);
    if (!ok) {
        kfree(b);
        return false;
    }
    size_t pos = 0;
    bool   hit = false;
    while (pos + 8 <= sz) {
        uint8_t *ent = b + pos;
        uint32_t ino = le32(ent);
        uint16_t rec = le16(ent + 4);
        uint8_t  nl  = ent[6];
        uint8_t  ft  = ent[7];
        if (rec < 8 || pos + rec > sz)
            break;
        if (ino == 0) {
            pos += rec;
            continue;
        }
        char name[VFS_NAME_MAX];
        size_t copy = nl;
        if (copy >= VFS_NAME_MAX)
            copy = VFS_NAME_MAX - 1;
        memcpy(name, ent + 8, copy);
        name[copy] = '\0';
        if (want) {
            if (strcmp(name, want) != 0) {
                pos += rec;
                continue;
            }
            *out_ino = ino;
            hit      = true;
        }
        if (want && hit) {
            uint8_t inb[512];
            if (read_inode_raw(c, ino, inb)) {
                uint32_t pb, nb;
                uint64_t isz;
                if (extent_root(inb, &pb, &nb, &isz)) {
                    out_node->inode        = ino;
                    out_node->size         = isz;
                    out_node->is_dir       = (ft == EXT4_FT_DIR) ? 1u : 0u;
                    out_node->data_block0  = pb;
                    out_node->data_blocks  = nb;
                    out_node->ee_len       = nb;
                } else {
                    hit = false;
                }
            } else
                hit = false;
            kfree(b);
            return hit;
        }
        pos += rec;
    }
    kfree(b);
    return false;
}

struct rd {
    vfs_dirent_t *e;
    size_t        max;
    int           n;
    ext4_ctx_t   *c;
};

static void walk_dir_all(ext4_ctx_t *c, uint32_t block, struct rd *L)
{
    size_t   sz = c->block_size;
    uint8_t *b  = (uint8_t *)kmalloc(sz);
    if (!b)
        return;
    uint64_t off = (uint64_t)c->part_lba * 512u + (uint64_t)block * c->block_size;
    if (!read_bytes(off, b, sz)) {
        kfree(b);
        return;
    }
    size_t pos = 0;
    while (pos + 8 <= sz && (size_t)L->n < L->max) {
        uint8_t *ent = b + pos;
        uint32_t ino = le32(ent);
        uint16_t rec = le16(ent + 4);
        uint8_t  nl  = ent[6];
        uint8_t  ft  = ent[7];
        if (rec < 8 || pos + rec > sz)
            break;
        if (ino == 0) {
            pos += rec;
            continue;
        }
        if (nl == 1 && ent[8] == '.') {
            pos += rec;
            continue;
        }
        if (nl == 2 && ent[8] == '.' && ent[9] == '.') {
            pos += rec;
            continue;
        }
        char name[VFS_NAME_MAX];
        size_t copy = nl;
        if (copy >= VFS_NAME_MAX)
            copy = VFS_NAME_MAX - 1;
        memcpy(name, ent + 8, copy);
        name[copy] = '\0';
        strncpy(L->e[L->n].name, name, VFS_NAME_MAX - 1);
        L->e[L->n].name[VFS_NAME_MAX - 1] = '\0';
        L->e[L->n].type  = (ft == EXT4_FT_DIR) ? VFS_DIR : VFS_FILE;
        L->e[L->n].inode = ino;
        uint8_t inb[512];
        uint64_t isz = 0;
        if (read_inode_raw(L->c, ino, inb)) {
            uint32_t pb, nb;
            if (extent_root(inb, &pb, &nb, &isz))
                (void)pb;
        }
        L->e[L->n].size = isz;
        L->n++;
        pos += rec;
    }
    kfree(b);
}

static int ext4_readdir(vfs_node_t *node, vfs_dirent_t *entries, size_t max)
{
    ext4_ctx_t *c = ctx_of(node);
    ext4_node_t *n = (ext4_node_t *)node->fs_data;
    if (!c || !n || !n->is_dir)
        return -EINVAL;
    struct rd L = { entries, max, 0, c };
    for (uint32_t i = 0; i < n->ee_len; i++)
        walk_dir_all(c, n->data_block0 + i, &L);
    return L.n;
}

static ssize_t ext4_read(vfs_node_t *node, uint64_t offset, void *buf, size_t size)
{
    ext4_ctx_t *c = ctx_of(node);
    ext4_node_t *n = (ext4_node_t *)node->fs_data;
    if (!c || !n || n->is_dir)
        return -EINVAL;
    if (offset >= n->size)
        return 0;
    size_t rem = (size_t)(n->size - offset);
    if (size > rem)
        size = rem;
    uint64_t base_byte =
        (uint64_t)c->part_lba * 512u + (uint64_t)n->data_block0 * c->block_size + offset;
    if (!read_bytes(base_byte, buf, size))
        return -EIO;
    return (ssize_t)size;
}

static int ext4_stat(vfs_node_t *node, vfs_stat_t *st)
{
    ext4_node_t *n = (ext4_node_t *)node->fs_data;
    if (!n)
        return -EINVAL;
    st->type        = n->is_dir ? VFS_DIR : VFS_FILE;
    st->size        = n->size;
    st->permissions = n->is_dir ? 0555 : 0444;
    st->inode       = n->inode;
    st->created     = 0;
    st->modified    = 0;
    return 0;
}

static vfs_node_t *ext4_lookup(vfs_node_t *parent, const char *name)
{
    ext4_ctx_t *c = ctx_of(parent);
    ext4_node_t *pd = (ext4_node_t *)parent->fs_data;
    if (!c || !pd || !pd->is_dir)
        return NULL;
    uint32_t ino = 0;
    ext4_node_t ch;
    memset(&ch, 0, sizeof(ch));
    bool found = false;
    for (uint32_t i = 0; i < pd->ee_len && !found; i++) {
        if (parse_dir_block(c, pd->data_block0 + i, name, &ino, &ch))
            found = true;
    }
    if (!found)
        return NULL;
    ext4_node_t *nd = (ext4_node_t *)kmalloc(sizeof(ext4_node_t));
    if (!nd)
        return NULL;
    memcpy(nd, &ch, sizeof(*nd));
    vfs_node_type_t t = nd->is_dir ? VFS_DIR : VFS_FILE;
    return vfs_make_child(parent, name, t, nd);
}

static vfs_ops_t ext4_ops = {
    .read    = ext4_read,
    .write   = NULL,
    .open    = NULL,
    .close   = NULL,
    .readdir = ext4_readdir,
    .mkdir   = NULL,
    .unlink  = NULL,
    .stat    = ext4_stat,
    .lookup  = ext4_lookup,
};

static bool probe_mbr(ext4_ctx_t *out)
{
    uint8_t sec[512];
    if (!block_dev_read(0, 1, sec))
        return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return false;
    for (int i = 0; i < 4; i++) {
        uint8_t *pe = sec + 0x1BE + i * 16;
        uint32_t lba = le32(pe + 8);
        if (lba == 0)
            continue;
        out->part_lba = lba;
        uint8_t sb[1024];
        if (!read_super(out, sb))
            continue;
        if (parse_super(sb, out))
            return true;
    }
    return false;
}

int ext4_try_mount(const char *mount_path)
{
    if (!block_dev_is_available())
        return -EIO;
    ext4_ctx_t *ctx = (ext4_ctx_t *)kmalloc(sizeof(ext4_ctx_t));
    if (!ctx)
        return -ENOMEM;
    memset(ctx, 0, sizeof(*ctx));
    uint8_t sb[1024];
    ctx->part_lba = 0;
    if (!read_super(ctx, sb) || !parse_super(sb, ctx)) {
        if (!probe_mbr(ctx)) {
            kfree(ctx);
            return -EINVAL;
        }
    }
    uint8_t root_in[512];
    if (!read_inode_raw(ctx, 2, root_in)) {
        kfree(ctx);
        return -EIO;
    }
    uint32_t pb, nb;
    uint64_t isz;
    if (!extent_root(root_in, &pb, &nb, &isz)) {
        kfree(ctx);
        return -ENOTSUP;
    }
    int rc = vfs_mount(mount_path, &ext4_ops, ctx);
    if (rc < 0) {
        kfree(ctx);
        return rc;
    }
    vfs_node_t *mp = vfs_resolve_path(mount_path);
    if (!mp) {
        kfree(ctx);
        return -ENOENT;
    }
    ext4_node_t *root = (ext4_node_t *)kmalloc(sizeof(ext4_node_t));
    if (!root) {
        kfree(ctx);
        return -ENOMEM;
    }
    memset(root, 0, sizeof(*root));
    root->inode       = 2;
    root->is_dir      = 1;
    root->size        = isz;
    root->data_block0 = pb;
    root->data_blocks = nb;
    root->ee_len      = nb;
    mp->fs_data       = root;
    klog("ext4: mounted at %s (root extent %u blocks)\n", mount_path, nb);
    return 0;
}
