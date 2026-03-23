#include "fs_fat32.h"
#include "vfs.h"
#include <kernel/drivers/block_dev.h>
#include <kernel/mm/slab.h>
#include <kernel/klog.h>
#include <lib/string.h>

typedef struct {
    uint32_t part_lba;
    uint32_t fat_begin_lba;
    uint32_t data_begin_lba;
    uint32_t root_cluster;
    uint32_t sectors_per_fat32;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t total_clusters;
} fat32_ctx_t;

typedef struct {
    uint32_t first_clus;
    uint32_t size;
    uint8_t  is_dir;
} fat32_node_t;

typedef struct {
    char     name[VFS_NAME_MAX];
    uint32_t first_clus;
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  is_volume;
} fat32_dirent_t;

static bool read_sec(uint32_t lba, void *buf)
{
    return block_dev_read((uint64_t)lba, 1, buf);
}

static fat32_ctx_t *ctx_of(const vfs_node_t *n)
{
    const vfs_node_t *p = n;
    while (p) {
        if (p->mount_data)
            return (fat32_ctx_t *)p->mount_data;
        p = p->parent;
    }
    return NULL;
}

static uint32_t fat_next(fat32_ctx_t *c, uint32_t cluster)
{
    if (cluster < 2 || cluster >= c->total_clusters + 2)
        return 0x0FFFFFF8;
    uint32_t fat_offset = cluster * 4u;
    uint32_t sec        = c->fat_begin_lba + fat_offset / c->bytes_per_sector;
    uint32_t off        = fat_offset % c->bytes_per_sector;
    uint8_t  buf[512];
    if (!read_sec(sec, buf))
        return 0x0FFFFFF8;
    if (off + 4 > c->bytes_per_sector)
        return 0x0FFFFFF8;
    uint32_t v = *(uint32_t *)(buf + off);
    return v & 0x0FFFFFFFu;
}

static uint32_t clus_to_lba(fat32_ctx_t *c, uint32_t clus)
{
    return c->data_begin_lba + (clus - 2u) * (uint32_t)c->sectors_per_cluster;
}

static void trim_83(const uint8_t *src, char *dst)
{
    char base[9];
    char ext[4];
    memcpy(base, src, 8);
    base[8] = '\0';
    int bi = 7;
    while (bi >= 0 && base[bi] == ' ')
        base[bi--] = '\0';
    memcpy(ext, src + 8, 3);
    ext[3] = '\0';
    int ei = 2;
    while (ei >= 0 && ext[ei] == ' ')
        ext[ei--] = '\0';
    if (ext[0]) {
        snprintf(dst, VFS_NAME_MAX, "%s.%s", base, ext);
    } else {
        strncpy(dst, base, VFS_NAME_MAX - 1);
        dst[VFS_NAME_MAX - 1] = '\0';
    }
    for (char *q = dst; *q; q++) {
        if (*q >= 'a' && *q <= 'z')
            *q = (char)(*q - 'a' + 'A');
    }
}

static int name_eq_fat(const char *a, const char *fat_norm)
{
    char tmp[VFS_NAME_MAX];
    strncpy(tmp, a, VFS_NAME_MAX - 1);
    tmp[VFS_NAME_MAX - 1] = '\0';
    for (char *q = tmp; *q; q++) {
        if (*q >= 'a' && *q <= 'z')
            *q = (char)(*q - 'a' + 'A');
    }
    return strcmp(tmp, fat_norm) == 0;
}

static bool scan_dir_cluster(fat32_ctx_t *c, uint32_t cluster,
                             const char *want, fat32_dirent_t *out)
{
    uint32_t lba = clus_to_lba(c, cluster);
    for (uint32_t s = 0; s < (uint32_t)c->sectors_per_cluster; s++) {
        uint8_t sec[512];
        if (!read_sec(lba + s, sec))
            return false;
        for (size_t off = 0; off + 32 <= c->bytes_per_sector; off += 32) {
            uint8_t *e = sec + off;
            if (e[0] == 0)
                return false;
            if (e[0] == 0xE5)
                continue;
            if (e[11] == 0x0F)
                continue;
            if ((e[11] & 0x08) != 0)
                continue;
            char nm[VFS_NAME_MAX];
            trim_83(e, nm);
            uint32_t cl_lo = (uint32_t)e[26] | ((uint32_t)e[27] << 8);
            uint32_t cl_hi = (uint32_t)e[20] | ((uint32_t)e[21] << 8);
            uint32_t fclus = cl_lo | (cl_hi << 16);
            uint32_t sz = *(uint32_t *)(e + 28);
            uint8_t  attr = e[11];
            bool is_dir = (attr & 0x10) != 0;
            if (want) {
                if (!name_eq_fat(want, nm))
                    continue;
                if (out) {
                    strncpy(out->name, nm, VFS_NAME_MAX - 1);
                    out->name[VFS_NAME_MAX - 1] = '\0';
                    out->first_clus = fclus;
                    out->size       = sz;
                    out->is_dir     = is_dir ? 1u : 0u;
                    out->is_volume  = 0;
                }
                return true;
            }
        }
    }
    return false;
}

static bool find_in_dir(fat32_ctx_t *c, uint32_t dir_clus,
                        const char *name, fat32_dirent_t *out)
{
    uint32_t cl = dir_clus;
    for (;;) {
        if (scan_dir_cluster(c, cl, name, out))
            return true;
        uint32_t nx = fat_next(c, cl);
        if (nx >= 0x0FFFFFF8u)
            break;
        cl = nx;
    }
    return false;
}

static void list_dir_cluster(fat32_ctx_t *c, uint32_t cluster,
                             void (*cb)(void *u, const fat32_dirent_t *),
                             void *user)
{
    uint32_t lba = clus_to_lba(c, cluster);
    for (uint32_t s = 0; s < (uint32_t)c->sectors_per_cluster; s++) {
        uint8_t sec[512];
        if (!read_sec(lba + s, sec))
            return;
        for (size_t off = 0; off + 32 <= c->bytes_per_sector; off += 32) {
            uint8_t *e = sec + off;
            if (e[0] == 0)
                return;
            if (e[0] == 0xE5)
                continue;
            if (e[11] == 0x0F)
                continue;
            if ((e[11] & 0x08) != 0)
                continue;
            fat32_dirent_t d;
            trim_83(e, d.name);
            uint32_t cl_lo = (uint32_t)e[26] | ((uint32_t)e[27] << 8);
            uint32_t cl_hi = (uint32_t)e[20] | ((uint32_t)e[21] << 8);
            d.first_clus = cl_lo | (cl_hi << 16);
            d.size       = *(uint32_t *)(e + 28);
            d.is_dir     = (e[11] & 0x10) ? 1u : 0u;
            d.is_volume  = 0;
            cb(user, &d);
        }
    }
}

struct list_cb {
    vfs_dirent_t *ents;
    size_t        max;
    int           n;
};

static void readdir_cb(void *u, const fat32_dirent_t *d)
{
    struct list_cb *L = (struct list_cb *)u;
    if ((size_t)L->n >= L->max)
        return;
    strncpy(L->ents[L->n].name, d->name, VFS_NAME_MAX - 1);
    L->ents[L->n].name[VFS_NAME_MAX - 1] = '\0';
    L->ents[L->n].type  = d->is_dir ? VFS_DIR : VFS_FILE;
    L->ents[L->n].inode = d->first_clus;
    L->ents[L->n].size  = d->size;
    L->n++;
}

static int fat32_readdir(vfs_node_t *node, vfs_dirent_t *entries, size_t max)
{
    fat32_ctx_t *c = ctx_of(node);
    fat32_node_t *fn = (fat32_node_t *)node->fs_data;
    if (!c || !fn || !fn->is_dir)
        return -EINVAL;

    struct list_cb L = { entries, max, 0 };
    uint32_t cl = fn->first_clus;
    for (;;) {
        list_dir_cluster(c, cl, readdir_cb, &L);
        uint32_t nx = fat_next(c, cl);
        if (nx >= 0x0FFFFFF8u)
            break;
        cl = nx;
        if (L.n >= (int)max)
            break;
    }
    return L.n;
}

static ssize_t fat32_read(vfs_node_t *node, uint64_t offset, void *buf, size_t size)
{
    fat32_ctx_t *c = ctx_of(node);
    fat32_node_t *fn = (fat32_node_t *)node->fs_data;
    if (!c || !fn || fn->is_dir)
        return -EINVAL;
    if (offset >= fn->size)
        return 0;
    size_t rem = fn->size - (uint32_t)offset;
    if (size > rem)
        size = rem;

    uint32_t clus_size = (uint32_t)c->bytes_per_sector * c->sectors_per_cluster;
    uint8_t *out       = (uint8_t *)buf;
    size_t   total     = 0;
    uint32_t cl        = fn->first_clus;
    uint64_t file_pos  = 0;

    while (file_pos + clus_size <= offset && cl >= 2u && cl < 0x0FFFFFF8u) {
        file_pos += clus_size;
        cl = fat_next(c, cl);
    }
    uint32_t skip = (uint32_t)(offset - file_pos);

    while (total < size && cl >= 2u && cl < 0x0FFFFFF8u) {
        uint32_t base = clus_to_lba(c, cl);
        for (uint32_t si = 0; si < (uint32_t)c->sectors_per_cluster && total < size;
             si++) {
            uint8_t sec[512];
            if (c->bytes_per_sector > sizeof(sec))
                return -EIO;
            if (!read_sec(base + si, sec))
                return -EIO;
            for (uint32_t o = 0; o < (uint32_t)c->bytes_per_sector && total < size;
                 o++) {
                if (skip > 0) {
                    skip--;
                    continue;
                }
                out[total++] = sec[o];
            }
        }
        cl = fat_next(c, cl);
    }
    return (ssize_t)total;
}

static int fat32_stat(vfs_node_t *node, vfs_stat_t *st)
{
    fat32_node_t *fn = (fat32_node_t *)node->fs_data;
    if (!fn)
        return -EINVAL;
    st->type        = fn->is_dir ? VFS_DIR : VFS_FILE;
    st->size        = fn->size;
    st->permissions = fn->is_dir ? 0555 : 0444;
    st->inode       = fn->first_clus;
    st->created     = 0;
    st->modified    = 0;
    return 0;
}

static vfs_node_t *fat32_lookup(vfs_node_t *parent, const char *name)
{
    fat32_ctx_t *c = ctx_of(parent);
    fat32_node_t *pdn = (fat32_node_t *)parent->fs_data;
    if (!c || !pdn || !pdn->is_dir)
        return NULL;
    fat32_dirent_t d;
    if (!find_in_dir(c, pdn->first_clus, name, &d))
        return NULL;
    fat32_node_t *chd = (fat32_node_t *)kmalloc(sizeof(fat32_node_t));
    if (!chd)
        return NULL;
    chd->first_clus = d.first_clus;
    chd->size       = d.size;
    chd->is_dir     = d.is_dir;
    vfs_node_type_t t = d.is_dir ? VFS_DIR : VFS_FILE;
    return vfs_make_child(parent, name, t, chd);
}

static vfs_ops_t fat32_ops = {
    .read    = fat32_read,
    .write   = NULL,
    .open    = NULL,
    .close   = NULL,
    .readdir = fat32_readdir,
    .mkdir   = NULL,
    .unlink  = NULL,
    .stat    = fat32_stat,
    .lookup  = fat32_lookup,
};

static bool parse_fat32_bpb(uint8_t *s0, fat32_ctx_t *out)
{
    uint16_t bps = *(uint16_t *)(s0 + 11);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096)
        return false;
    uint8_t spc = s0[13];
    if (spc == 0)
        return false;
    uint16_t reserved = *(uint16_t *)(s0 + 14);
    uint8_t  nfat     = s0[16];
    uint16_t root_ent = *(uint16_t *)(s0 + 17);
    uint32_t spf32    = *(uint32_t *)(s0 + 36);
    if (root_ent != 0 || spf32 == 0)
        return false;
    uint32_t root_clus = *(uint32_t *)(s0 + 44);
    uint32_t total_sec = *(uint32_t *)(s0 + 32);
    if (total_sec == 0)
        total_sec = *(uint16_t *)(s0 + 19);

    out->bytes_per_sector     = bps;
    out->sectors_per_cluster  = spc;
    out->sectors_per_fat32    = spf32;
    out->root_cluster         = root_clus;
    out->fat_begin_lba        = reserved;
    out->data_begin_lba       = reserved + nfat * spf32;
    uint32_t data_sec = total_sec - out->data_begin_lba;
    out->total_clusters = data_sec / spc;
    (void)root_ent;
    return root_clus >= 2;
}

static bool probe_partition(fat32_ctx_t *out)
{
    uint8_t sec[512];
    if (!read_sec(0, sec))
        return false;
    if (sec[510] == 0x55 && sec[511] == 0xAA && sec[450] != 0xEE) {
        for (int i = 0; i < 4; i++) {
            uint8_t *pe = sec + 0x1BE + i * 16;
            uint8_t typ = pe[4];
            if (typ != 0x0C && typ != 0x0B)
                continue;
            uint32_t lba = *(uint32_t *)(pe + 8);
            if (!read_sec(lba, sec))
                continue;
            out->part_lba = lba;
            if (parse_fat32_bpb(sec, out)) {
                uint32_t p = out->part_lba;
                out->fat_begin_lba += p;
                out->data_begin_lba += p;
                return true;
            }
        }
    }
    out->part_lba = 0;
    if (!parse_fat32_bpb(sec, out))
        return false;
    out->fat_begin_lba += out->part_lba;
    out->data_begin_lba += out->part_lba;
    return true;
}

int fat32_try_mount(const char *mount_path)
{
    if (!block_dev_is_available())
        return -EIO;
    fat32_ctx_t *ctx = (fat32_ctx_t *)kmalloc(sizeof(fat32_ctx_t));
    if (!ctx)
        return -ENOMEM;
    memset(ctx, 0, sizeof(*ctx));
    if (!probe_partition(ctx)) {
        kfree(ctx);
        return -EINVAL;
    }
    int rc = vfs_mount(mount_path, &fat32_ops, ctx);
    if (rc < 0) {
        kfree(ctx);
        return rc;
    }
    vfs_node_t *mp = vfs_resolve_path(mount_path);
    if (!mp) {
        kfree(ctx);
        return -ENOENT;
    }
    fat32_node_t *root = (fat32_node_t *)kmalloc(sizeof(fat32_node_t));
    if (!root) {
        kfree(ctx);
        return -ENOMEM;
    }
    root->first_clus = ctx->root_cluster;
    root->size       = 0;
    root->is_dir     = 1;
    mp->fs_data      = root;
    klog("fat32: mounted at %s (root_clus=%u)\n", mount_path, ctx->root_cluster);
    return 0;
}
