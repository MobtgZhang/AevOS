#include "aevosfs.h"
#include <kernel/mm/slab.h>
#include <kernel/klog.h>
#include <lib/string.h>

/* ── Global state ─────────────────────────────────────────────────── */

static aevosfs_dev_read_fn  dev_read;
static aevosfs_dev_write_fn dev_write;
static aevosfs_superblock_t sb;
static aevosfs_inode_t      inode_table[AEVOSFS_MAX_INODES];
static spinlock_t          fs_lock = SPINLOCK_INIT;
static uint64_t            next_inode_id = 1;

/* ── Block allocation (simple bitmap-free bump allocator for log) ── */

static uint64_t alloc_block(void)
{
    if (sb.free_blocks == 0)
        return 0;
    uint64_t blk = sb.log_head;
    sb.log_head++;
    sb.free_blocks--;
    return blk;
}

/* ── Device I/O wrappers ──────────────────────────────────────────── */

static int read_block(uint64_t block, void *buf)
{
    ssize_t r = dev_read(block, buf, 1);
    return (r == 1) ? 0 : -EIO;
}

static int write_block(uint64_t block, const void *buf)
{
    ssize_t r = dev_write(block, buf, 1);
    return (r == 1) ? 0 : -EIO;
}

/* ── Inode management ─────────────────────────────────────────────── */

static aevosfs_inode_t *find_inode(uint64_t id)
{
    for (size_t i = 0; i < AEVOSFS_MAX_INODES; i++) {
        if (inode_table[i].id == id)
            return &inode_table[i];
    }
    return NULL;
}

static aevosfs_inode_t *alloc_inode(aevosfs_inode_type_t type)
{
    for (size_t i = 0; i < AEVOSFS_MAX_INODES; i++) {
        if (inode_table[i].id == 0) {
            memset(&inode_table[i], 0, sizeof(aevosfs_inode_t));
            inode_table[i].id   = next_inode_id++;
            inode_table[i].type = type;
            inode_table[i].permissions = 0755;
            inode_table[i].link_count  = 1;
            return &inode_table[i];
        }
    }
    return NULL;
}

/* ── Log-structured write ─────────────────────────────────────────── */

static int log_append_write(aevosfs_inode_t *inode, uint64_t offset,
                            const void *buf, uint32_t length, uint64_t data_blk)
{
    (void)offset;
    (void)buf;
    aevosfs_log_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.entry_type = AEVOSFS_LOG_ENTRY_WRITE;
    entry.inode_id   = inode->id;
    entry.offset     = offset;
    entry.length     = length;
    entry.data_block = data_blk;
    entry.timestamp  = 0; /* TODO: real timestamp from timer */

    uint64_t log_blk = alloc_block();
    if (!log_blk) return -ENOSPC;

    uint8_t block_buf[AEVOSFS_BLOCK_SIZE];
    memset(block_buf, 0, sizeof(block_buf));
    memcpy(block_buf, &entry, sizeof(entry));
    return write_block(log_blk, block_buf);
}

/* ── Resolve path within AEVOSFS ───────────────────────────────────── */

/*
 * Simple path walk using directory entries stored in inode data blocks.
 * Returns the inode for the final path component.
 */
static aevosfs_inode_t *resolve_path_internal(const char *path)
{
    aevosfs_inode_t *cur = find_inode(sb.root_inode);
    if (!cur) return NULL;
    if (!path || *path == '\0' || (path[0] == '/' && path[1] == '\0'))
        return cur;

    const char *p = path;
    if (*p == '/') p++;

    char component[AEVOSFS_MAX_NAME];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        size_t len = 0;
        while (*p && *p != '/' && len < AEVOSFS_MAX_NAME - 1)
            component[len++] = *p++;
        component[len] = '\0';

        if (cur->type != AEVOSFS_DIR)
            return NULL;

        /* scan directory data blocks for the named entry */
        bool found = false;
        for (int b = 0; b < AEVOSFS_DIRECT_BLOCKS && cur->block_list[b]; b++) {
            uint8_t block_buf[AEVOSFS_BLOCK_SIZE];
            if (read_block(cur->block_list[b], block_buf) != 0)
                continue;

            size_t entries_per_block = AEVOSFS_BLOCK_SIZE / sizeof(aevosfs_dir_entry_t);
            aevosfs_dir_entry_t *de = (aevosfs_dir_entry_t *)block_buf;
            for (size_t i = 0; i < entries_per_block; i++) {
                if (de[i].inode_id && strcmp(de[i].name, component) == 0) {
                    cur = find_inode(de[i].inode_id);
                    if (!cur) return NULL;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) return NULL;
    }
    return cur;
}

/* ── Add a directory entry to parent inode ─────────────────────────── */

static int add_dir_entry(aevosfs_inode_t *parent, const char *name,
                         uint64_t child_inode_id, aevosfs_inode_type_t type)
{
    size_t entries_per_block = AEVOSFS_BLOCK_SIZE / sizeof(aevosfs_dir_entry_t);

    /* scan existing blocks for a free slot */
    for (int b = 0; b < AEVOSFS_DIRECT_BLOCKS && parent->block_list[b]; b++) {
        uint8_t block_buf[AEVOSFS_BLOCK_SIZE];
        if (read_block(parent->block_list[b], block_buf) != 0)
            continue;

        aevosfs_dir_entry_t *de = (aevosfs_dir_entry_t *)block_buf;
        for (size_t i = 0; i < entries_per_block; i++) {
            if (de[i].inode_id == 0) {
                de[i].inode_id = child_inode_id;
                strncpy(de[i].name, name, AEVOSFS_MAX_NAME - 1);
                de[i].name[AEVOSFS_MAX_NAME - 1] = '\0';
                de[i].type = type;
                return write_block(parent->block_list[b], block_buf);
            }
        }
    }

    /* need a new block */
    for (int b = 0; b < AEVOSFS_DIRECT_BLOCKS; b++) {
        if (parent->block_list[b] == 0) {
            uint64_t blk = alloc_block();
            if (!blk) return -ENOSPC;

            uint8_t block_buf[AEVOSFS_BLOCK_SIZE];
            memset(block_buf, 0, sizeof(block_buf));
            aevosfs_dir_entry_t *de = (aevosfs_dir_entry_t *)block_buf;
            de[0].inode_id = child_inode_id;
            strncpy(de[0].name, name, AEVOSFS_MAX_NAME - 1);
            de[0].name[AEVOSFS_MAX_NAME - 1] = '\0';
            de[0].type = type;

            parent->block_list[b] = blk;
            return write_block(blk, block_buf);
        }
    }
    return -ENOSPC;
}

/* ── Public API ───────────────────────────────────────────────────── */

int aevosfs_init(aevosfs_dev_read_fn read_fn, aevosfs_dev_write_fn write_fn)
{
    if (!read_fn || !write_fn) return -EINVAL;

    dev_read  = read_fn;
    dev_write = write_fn;
    memset(inode_table, 0, sizeof(inode_table));
    next_inode_id = 1;

    klog("aevosfs: driver initialized\n");
    return 0;
}

int aevosfs_mount(void)
{
    spin_lock(&fs_lock);

    /* try to read existing superblock */
    uint8_t block_buf[AEVOSFS_BLOCK_SIZE];
    if (read_block(0, block_buf) == 0) {
        memcpy(&sb, block_buf, sizeof(sb));
    }

    if (sb.magic != AEVOSFS_MAGIC) {
        klog("aevosfs: no valid superblock, creating fresh filesystem\n");

        memset(&sb, 0, sizeof(sb));
        sb.magic        = AEVOSFS_MAGIC;
        sb.version      = AEVOSFS_VERSION;
        sb.block_size   = AEVOSFS_BLOCK_SIZE;
        sb.total_blocks = 65536; /* default ~256 MB */
        sb.free_blocks  = sb.total_blocks - 2; /* block 0=sb, block 1=root dir */
        sb.log_head     = 2;

        /* create root inode */
        aevosfs_inode_t *root = alloc_inode(AEVOSFS_DIR);
        if (!root) {
            spin_unlock(&fs_lock);
            return -ENOMEM;
        }
        sb.root_inode = root->id;

        /* allocate a data block for root dir entries */
        root->block_list[0] = 1;
        memset(block_buf, 0, sizeof(block_buf));
        write_block(1, block_buf);

        /* write superblock */
        memset(block_buf, 0, sizeof(block_buf));
        memcpy(block_buf, &sb, sizeof(sb));
        write_block(0, block_buf);
    } else {
        klog("aevosfs: superblock loaded (v%u, %llu blocks)\n",
             sb.version, sb.total_blocks);
        /* reconstruct root inode in memory */
        if (!find_inode(sb.root_inode)) {
            aevosfs_inode_t *root = alloc_inode(AEVOSFS_DIR);
            if (root) root->id = sb.root_inode;
        }
    }

    spin_unlock(&fs_lock);
    klog("aevosfs: mounted\n");
    return 0;
}

int aevosfs_create(const char *path, aevosfs_inode_type_t type)
{
    if (!path || *path != '/') return -EINVAL;

    spin_lock(&fs_lock);

    /* split into parent path + name */
    char parent_path[VFS_PATH_MAX];
    char name[AEVOSFS_MAX_NAME];

    strncpy(parent_path, path, VFS_PATH_MAX - 1);
    parent_path[VFS_PATH_MAX - 1] = '\0';

    char *last_slash = strrchr(parent_path, '/');
    if (!last_slash) { spin_unlock(&fs_lock); return -EINVAL; }

    if (last_slash == parent_path) {
        strncpy(name, path + 1, AEVOSFS_MAX_NAME - 1);
        parent_path[1] = '\0';
    } else {
        strncpy(name, last_slash + 1, AEVOSFS_MAX_NAME - 1);
        *last_slash = '\0';
    }
    name[AEVOSFS_MAX_NAME - 1] = '\0';

    aevosfs_inode_t *parent = resolve_path_internal(parent_path);
    if (!parent || parent->type != AEVOSFS_DIR) {
        spin_unlock(&fs_lock);
        return -ENOENT;
    }

    aevosfs_inode_t *inode = alloc_inode(type);
    if (!inode) { spin_unlock(&fs_lock); return -ENOMEM; }

    int rc = add_dir_entry(parent, name, inode->id, type);
    if (rc < 0) {
        inode->id = 0; /* free the inode slot */
        spin_unlock(&fs_lock);
        return rc;
    }

    spin_unlock(&fs_lock);
    return 0;
}

ssize_t aevosfs_read(aevosfs_inode_t *inode, uint64_t offset, void *buf, size_t size)
{
    if (!inode || !buf) return -EINVAL;
    if (offset >= inode->size) return 0;

    size_t remaining = (size_t)(inode->size - offset);
    if (size > remaining) size = remaining;

    uint8_t *out = (uint8_t *)buf;
    size_t total_read = 0;

    while (total_read < size) {
        uint64_t block_idx    = (offset + total_read) / AEVOSFS_BLOCK_SIZE;
        uint64_t block_offset = (offset + total_read) % AEVOSFS_BLOCK_SIZE;

        if (block_idx >= AEVOSFS_DIRECT_BLOCKS)
            break; /* indirect blocks not yet supported */

        uint64_t phys_blk = inode->block_list[block_idx];
        if (!phys_blk) break;

        uint8_t block_buf[AEVOSFS_BLOCK_SIZE];
        if (read_block(phys_blk, block_buf) != 0)
            return -EIO;

        size_t chunk = AEVOSFS_BLOCK_SIZE - (size_t)block_offset;
        if (chunk > size - total_read)
            chunk = size - total_read;

        memcpy(out + total_read, block_buf + block_offset, chunk);
        total_read += chunk;
    }

    return (ssize_t)total_read;
}

ssize_t aevosfs_write(aevosfs_inode_t *inode, uint64_t offset,
                     const void *buf, size_t size)
{
    if (!inode || !buf) return -EINVAL;

    spin_lock(&fs_lock);

    const uint8_t *in = (const uint8_t *)buf;
    size_t total_written = 0;

    while (total_written < size) {
        uint64_t block_idx    = (offset + total_written) / AEVOSFS_BLOCK_SIZE;
        uint64_t block_offset = (offset + total_written) % AEVOSFS_BLOCK_SIZE;

        if (block_idx >= AEVOSFS_DIRECT_BLOCKS) {
            spin_unlock(&fs_lock);
            return (ssize_t)total_written;
        }

        /* allocate block if needed */
        if (!inode->block_list[block_idx]) {
            uint64_t blk = alloc_block();
            if (!blk) {
                spin_unlock(&fs_lock);
                return (total_written > 0) ? (ssize_t)total_written : -ENOSPC;
            }
            inode->block_list[block_idx] = blk;
        }

        /* log-structured: write data to a new block, record in log */
        uint8_t block_buf[AEVOSFS_BLOCK_SIZE];

        /* read-modify-write for partial block writes */
        if (block_offset != 0 || (size - total_written) < AEVOSFS_BLOCK_SIZE) {
            if (read_block(inode->block_list[block_idx], block_buf) != 0)
                memset(block_buf, 0, sizeof(block_buf));
        }

        size_t chunk = AEVOSFS_BLOCK_SIZE - (size_t)block_offset;
        if (chunk > size - total_written)
            chunk = size - total_written;

        memcpy(block_buf + block_offset, in + total_written, chunk);

        /* write data block */
        uint64_t new_blk = alloc_block();
        if (!new_blk) {
            spin_unlock(&fs_lock);
            return (total_written > 0) ? (ssize_t)total_written : -ENOSPC;
        }
        if (write_block(new_blk, block_buf) != 0) {
            spin_unlock(&fs_lock);
            return -EIO;
        }

        /* update inode to point to new block */
        inode->block_list[block_idx] = new_blk;

        /* append log entry */
        log_append_write(inode, offset + total_written, in + total_written,
                         (uint32_t)chunk, new_blk);

        total_written += chunk;
    }

    uint64_t new_end = offset + total_written;
    if (new_end > inode->size)
        inode->size = new_end;

    spin_unlock(&fs_lock);
    return (ssize_t)total_written;
}

int aevosfs_list_dir(aevosfs_inode_t *inode, aevosfs_dir_entry_t *entries, size_t max)
{
    if (!inode || !entries || inode->type != AEVOSFS_DIR)
        return -EINVAL;

    int count = 0;
    size_t entries_per_block = AEVOSFS_BLOCK_SIZE / sizeof(aevosfs_dir_entry_t);

    for (int b = 0; b < AEVOSFS_DIRECT_BLOCKS && inode->block_list[b]; b++) {
        uint8_t block_buf[AEVOSFS_BLOCK_SIZE];
        if (read_block(inode->block_list[b], block_buf) != 0)
            continue;

        aevosfs_dir_entry_t *de = (aevosfs_dir_entry_t *)block_buf;
        for (size_t i = 0; i < entries_per_block && (size_t)count < max; i++) {
            if (de[i].inode_id != 0) {
                memcpy(&entries[count], &de[i], sizeof(aevosfs_dir_entry_t));
                count++;
            }
        }
    }
    return count;
}

int aevosfs_checkpoint(void)
{
    spin_lock(&fs_lock);

    /* write all in-memory inodes to disk */
    for (size_t i = 0; i < AEVOSFS_MAX_INODES; i++) {
        if (inode_table[i].id == 0) continue;

        uint64_t blk = alloc_block();
        if (!blk) {
            spin_unlock(&fs_lock);
            return -ENOSPC;
        }

        uint8_t block_buf[AEVOSFS_BLOCK_SIZE];
        memset(block_buf, 0, sizeof(block_buf));
        memcpy(block_buf, &inode_table[i], sizeof(aevosfs_inode_t));
        if (write_block(blk, block_buf) != 0) {
            spin_unlock(&fs_lock);
            return -EIO;
        }
    }

    sb.checkpoint_block = sb.log_head;

    /* rewrite superblock */
    uint8_t sb_buf[AEVOSFS_BLOCK_SIZE];
    memset(sb_buf, 0, sizeof(sb_buf));
    memcpy(sb_buf, &sb, sizeof(sb));
    int rc = write_block(0, sb_buf);

    spin_unlock(&fs_lock);
    klog("aevosfs: checkpoint complete (log_head=%llu)\n", sb.log_head);
    return rc;
}

/* ── VFS bridge ───────────────────────────────────────────────────── */

static ssize_t aevosfs_vfs_read(vfs_node_t *node, uint64_t offset,
                                void *buf, size_t size)
{
    aevosfs_inode_t *inode = (aevosfs_inode_t *)node->fs_data;
    if (!inode) return -EINVAL;
    return aevosfs_read(inode, offset, buf, size);
}

static ssize_t aevosfs_vfs_write(vfs_node_t *node, uint64_t offset,
                                 const void *buf, size_t size)
{
    aevosfs_inode_t *inode = (aevosfs_inode_t *)node->fs_data;
    if (!inode) return -EINVAL;
    return aevosfs_write(inode, offset, buf, size);
}

static int aevosfs_vfs_open(vfs_node_t *node, uint32_t flags)
{
    (void)node;
    (void)flags;
    return 0;
}

static int aevosfs_vfs_close(vfs_node_t *node)
{
    (void)node;
    return 0;
}

static int aevosfs_vfs_readdir(vfs_node_t *node, vfs_dirent_t *entries,
                               size_t max_entries)
{
    aevosfs_inode_t *inode = (aevosfs_inode_t *)node->fs_data;
    if (!inode) return -EINVAL;

    aevosfs_dir_entry_t *tmp = (aevosfs_dir_entry_t *)kmalloc(
        max_entries * sizeof(aevosfs_dir_entry_t));
    if (!tmp) return -ENOMEM;

    int n = aevosfs_list_dir(inode, tmp, max_entries);
    for (int i = 0; i < n; i++) {
        strncpy(entries[i].name, tmp[i].name, VFS_NAME_MAX - 1);
        entries[i].name[VFS_NAME_MAX - 1] = '\0';
        entries[i].type  = (tmp[i].type == AEVOSFS_DIR) ? VFS_DIR : VFS_FILE;
        entries[i].inode = tmp[i].inode_id;
        entries[i].size  = 0;
        aevosfs_inode_t *child = find_inode(tmp[i].inode_id);
        if (child) entries[i].size = child->size;
    }

    kfree(tmp);
    return n;
}

static int aevosfs_vfs_mkdir(vfs_node_t *parent, const char *name)
{
    (void)parent;
    (void)name;
    return 0; /* actual creation done via aevosfs_create */
}

static int aevosfs_vfs_stat(vfs_node_t *node, vfs_stat_t *st)
{
    aevosfs_inode_t *inode = (aevosfs_inode_t *)node->fs_data;
    if (!inode) return -EINVAL;

    st->type        = (inode->type == AEVOSFS_DIR) ? VFS_DIR : VFS_FILE;
    st->size        = inode->size;
    st->permissions = inode->permissions;
    st->inode       = inode->id;
    st->created     = inode->created;
    st->modified    = inode->modified;
    return 0;
}

static vfs_ops_t aevosfs_ops = {
    .read    = aevosfs_vfs_read,
    .write   = aevosfs_vfs_write,
    .open    = aevosfs_vfs_open,
    .close   = aevosfs_vfs_close,
    .readdir = aevosfs_vfs_readdir,
    .mkdir   = aevosfs_vfs_mkdir,
    .unlink  = NULL,
    .stat    = aevosfs_vfs_stat,
};

vfs_ops_t *aevosfs_get_vfs_ops(void)
{
    return &aevosfs_ops;
}

static uint32_t g_aevosfs_features = AEVOSFS_FEAT_JOURNAL_META;

uint32_t aevosfs_feature_flags(void)
{
    return g_aevosfs_features;
}

int aevosfs_journal_stage_meta(const char *reason)
{
    klog("aevosfs: journal meta stage (%s) log_head=%llu\n",
         reason ? reason : "null",
         (unsigned long long)sb.log_head);
    return 0;
}

bool aevosfs_cow_blocks_enabled(void)
{
    return (g_aevosfs_features & AEVOSFS_FEAT_COW_BLOCKS) != 0;
}
