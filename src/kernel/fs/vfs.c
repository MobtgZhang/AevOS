#include "vfs.h"
#include <aevos/config.h>
#include <kernel/mm/slab.h>
#include <kernel/klog.h>
#include <lib/string.h>

/* ── File descriptor table ────────────────────────────────────────── */

typedef struct {
    vfs_node_t *node;
    uint32_t    flags;
    uint64_t    offset;
    bool        in_use;
} vfs_fd_entry_t;

static vfs_fd_entry_t fd_table[MAX_OPEN_FILES];
static vfs_node_t    *vfs_root;
static spinlock_t     vfs_lock = SPINLOCK_INIT;

/* ── Helpers ──────────────────────────────────────────────────────── */

static vfs_fd_t alloc_fd(void)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!fd_table[i].in_use) {
            fd_table[i].in_use = true;
            return (vfs_fd_t)i;
        }
    }
    return -ENOMEM;
}

static vfs_node_t *create_node(const char *name, vfs_node_type_t type)
{
    vfs_node_t *node = (vfs_node_t *)kcalloc(1, sizeof(vfs_node_t));
    if (!node) return NULL;

    strncpy(node->name, name, VFS_NAME_MAX - 1);
    node->name[VFS_NAME_MAX - 1] = '\0';
    node->type = type;
    return node;
}

static void add_child(vfs_node_t *parent, vfs_node_t *child)
{
    child->parent = parent;
    child->next_sibling = parent->children;
    parent->children = child;
}

static vfs_node_t *find_child(vfs_node_t *parent, const char *name)
{
    for (vfs_node_t *c = parent->children; c; c = c->next_sibling) {
        if (strcmp(c->name, name) == 0)
            return c;
    }
    return NULL;
}

/*
 * Split a path into components and walk the tree.
 * E.g. "/usr/lib/foo" → "usr", "lib", "foo"
 */
vfs_node_t *vfs_resolve_path(const char *path)
{
    if (!path || *path != '/')
        return NULL;

    vfs_node_t *cur = vfs_root;
    path++; /* skip leading '/' */

    char component[VFS_NAME_MAX];
    while (*path) {
        /* skip multiple slashes */
        while (*path == '/') path++;
        if (!*path) break;

        /* extract next component */
        size_t len = 0;
        while (*path && *path != '/' && len < VFS_NAME_MAX - 1)
            component[len++] = *path++;
        component[len] = '\0';

        if (strcmp(component, ".") == 0)
            continue;
        if (strcmp(component, "..") == 0) {
            if (cur->parent) cur = cur->parent;
            continue;
        }

        vfs_node_t *child = find_child(cur, component);
        if (!child) return NULL;
        cur = child;
    }
    return cur;
}

/* ── Get effective ops for a node (check mount overlay) ───────────── */

static vfs_ops_t *get_ops(vfs_node_t *node)
{
    if (node->mount_ops) return node->mount_ops;
    if (node->ops)       return node->ops;
    /* walk up to find mounted ancestor */
    vfs_node_t *p = node->parent;
    while (p) {
        if (p->mount_ops) return p->mount_ops;
        if (p->ops)       return p->ops;
        p = p->parent;
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────── */

int vfs_init(void)
{
    memset(fd_table, 0, sizeof(fd_table));
    vfs_root = create_node("/", VFS_DIR);
    if (!vfs_root) {
        klog("vfs: failed to create root node\n");
        return -ENOMEM;
    }
    klog("vfs: initialized\n");
    return 0;
}

int vfs_mount(const char *path, vfs_ops_t *fs_ops, void *fs_data)
{
    if (!path || !fs_ops) return -EINVAL;

    spin_lock(&vfs_lock);

    vfs_node_t *mount_point;
    if (strcmp(path, "/") == 0) {
        mount_point = vfs_root;
    } else {
        mount_point = vfs_resolve_path(path);
        if (!mount_point) {
            /* create intermediate dirs */
            vfs_node_t *cur = vfs_root;
            const char *p = path + 1;
            char comp[VFS_NAME_MAX];

            while (*p) {
                while (*p == '/') p++;
                if (!*p) break;
                size_t len = 0;
                while (*p && *p != '/' && len < VFS_NAME_MAX - 1)
                    comp[len++] = *p++;
                comp[len] = '\0';

                vfs_node_t *child = find_child(cur, comp);
                if (!child) {
                    child = create_node(comp, VFS_DIR);
                    if (!child) { spin_unlock(&vfs_lock); return -ENOMEM; }
                    add_child(cur, child);
                }
                cur = child;
            }
            mount_point = cur;
        }
    }

    mount_point->mount_ops  = fs_ops;
    mount_point->mount_data = fs_data;

    spin_unlock(&vfs_lock);
    klog("vfs: mounted filesystem at %s\n", path);
    return 0;
}

vfs_fd_t vfs_open(const char *path, uint32_t flags)
{
    spin_lock(&vfs_lock);

    vfs_node_t *node = vfs_resolve_path(path);
    if (!node && (flags & VFS_O_CREATE)) {
        /* find parent and create file */
        char parent_path[VFS_PATH_MAX];
        char filename[VFS_NAME_MAX];

        /* split into parent + basename */
        strncpy(parent_path, path, VFS_PATH_MAX - 1);
        parent_path[VFS_PATH_MAX - 1] = '\0';

        char *last_slash = strrchr(parent_path, '/');
        if (!last_slash) {
            spin_unlock(&vfs_lock);
            return -EINVAL;
        }

        if (last_slash == parent_path) {
            /* parent is root */
            strncpy(filename, path + 1, VFS_NAME_MAX - 1);
            filename[VFS_NAME_MAX - 1] = '\0';
            node = create_node(filename, VFS_FILE);
            if (!node) { spin_unlock(&vfs_lock); return -ENOMEM; }
            add_child(vfs_root, node);
        } else {
            *last_slash = '\0';
            strncpy(filename, last_slash + 1, VFS_NAME_MAX - 1);
            filename[VFS_NAME_MAX - 1] = '\0';

            vfs_node_t *parent = vfs_resolve_path(parent_path);
            if (!parent || parent->type != VFS_DIR) {
                spin_unlock(&vfs_lock);
                return -ENOENT;
            }
            node = create_node(filename, VFS_FILE);
            if (!node) { spin_unlock(&vfs_lock); return -ENOMEM; }
            node->ops = parent->ops;
            add_child(parent, node);
        }
    }

    if (!node) {
        spin_unlock(&vfs_lock);
        return -ENOENT;
    }

    vfs_fd_t fd = alloc_fd();
    if (fd < 0) {
        spin_unlock(&vfs_lock);
        return fd;
    }

    fd_table[fd].node   = node;
    fd_table[fd].flags  = flags;
    fd_table[fd].offset = 0;

    if (flags & VFS_O_TRUNC)
        node->size = 0;

    vfs_ops_t *ops = get_ops(node);
    if (ops && ops->open)
        ops->open(node, flags);

    spin_unlock(&vfs_lock);
    return fd;
}

int vfs_close(vfs_fd_t fd)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table[fd].in_use)
        return -EINVAL;

    spin_lock(&vfs_lock);

    vfs_node_t *node = fd_table[fd].node;
    vfs_ops_t  *ops  = get_ops(node);
    if (ops && ops->close)
        ops->close(node);

    fd_table[fd].in_use = false;
    fd_table[fd].node   = NULL;
    fd_table[fd].offset = 0;
    fd_table[fd].flags  = 0;

    spin_unlock(&vfs_lock);
    return 0;
}

ssize_t vfs_read(vfs_fd_t fd, void *buf, size_t size)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table[fd].in_use)
        return -EINVAL;
    if (!buf || size == 0)
        return -EINVAL;

    vfs_node_t *node = fd_table[fd].node;
    vfs_ops_t  *ops  = get_ops(node);
    if (!ops || !ops->read)
        return -ENOTSUP;

    ssize_t n = ops->read(node, fd_table[fd].offset, buf, size);
    if (n > 0)
        fd_table[fd].offset += (uint64_t)n;
    return n;
}

ssize_t vfs_write(vfs_fd_t fd, const void *buf, size_t size)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table[fd].in_use)
        return -EINVAL;
    if (!buf || size == 0)
        return -EINVAL;

    vfs_node_t *node = fd_table[fd].node;
    vfs_ops_t  *ops  = get_ops(node);
    if (!ops || !ops->write)
        return -ENOTSUP;

    if (fd_table[fd].flags & VFS_O_APPEND)
        fd_table[fd].offset = node->size;

    ssize_t n = ops->write(node, fd_table[fd].offset, buf, size);
    if (n > 0) {
        fd_table[fd].offset += (uint64_t)n;
        if (fd_table[fd].offset > node->size)
            node->size = fd_table[fd].offset;
    }
    return n;
}

int vfs_mkdir(const char *path)
{
    if (!path || *path != '/') return -EINVAL;

    spin_lock(&vfs_lock);

    /* check if already exists */
    vfs_node_t *existing = vfs_resolve_path(path);
    if (existing) {
        spin_unlock(&vfs_lock);
        return -EEXIST;
    }

    /* find parent */
    char parent_path[VFS_PATH_MAX];
    char dirname[VFS_NAME_MAX];

    strncpy(parent_path, path, VFS_PATH_MAX - 1);
    parent_path[VFS_PATH_MAX - 1] = '\0';
    char *last_slash = strrchr(parent_path, '/');
    if (!last_slash) { spin_unlock(&vfs_lock); return -EINVAL; }

    vfs_node_t *parent;
    if (last_slash == parent_path) {
        parent = vfs_root;
        strncpy(dirname, path + 1, VFS_NAME_MAX - 1);
    } else {
        *last_slash = '\0';
        strncpy(dirname, last_slash + 1, VFS_NAME_MAX - 1);
        parent = vfs_resolve_path(parent_path);
    }
    dirname[VFS_NAME_MAX - 1] = '\0';

    if (!parent || parent->type != VFS_DIR) {
        spin_unlock(&vfs_lock);
        return -ENOENT;
    }

    /* delegate to fs driver if present */
    vfs_ops_t *ops = get_ops(parent);
    if (ops && ops->mkdir) {
        int rc = ops->mkdir(parent, dirname);
        if (rc < 0) { spin_unlock(&vfs_lock); return rc; }
    }

    vfs_node_t *dir = create_node(dirname, VFS_DIR);
    if (!dir) { spin_unlock(&vfs_lock); return -ENOMEM; }
    dir->ops = parent->ops;
    add_child(parent, dir);

    spin_unlock(&vfs_lock);
    return 0;
}

int vfs_readdir(vfs_fd_t fd, vfs_dirent_t *entries, size_t max_entries)
{
    if (fd < 0 || fd >= MAX_OPEN_FILES || !fd_table[fd].in_use)
        return -EINVAL;
    if (!entries || max_entries == 0)
        return -EINVAL;

    vfs_node_t *node = fd_table[fd].node;
    if (node->type != VFS_DIR)
        return -EINVAL;

    vfs_ops_t *ops = get_ops(node);
    if (ops && ops->readdir)
        return ops->readdir(node, entries, max_entries);

    /* fallback: iterate VFS children */
    int count = 0;
    for (vfs_node_t *c = node->children; c && (size_t)count < max_entries; c = c->next_sibling) {
        strncpy(entries[count].name, c->name, VFS_NAME_MAX - 1);
        entries[count].name[VFS_NAME_MAX - 1] = '\0';
        entries[count].type  = c->type;
        entries[count].inode = c->inode;
        entries[count].size  = c->size;
        count++;
    }
    return count;
}

int vfs_stat(const char *path, vfs_stat_t *st)
{
    if (!path || !st) return -EINVAL;

    vfs_node_t *node = vfs_resolve_path(path);
    if (!node) return -ENOENT;

    vfs_ops_t *ops = get_ops(node);
    if (ops && ops->stat)
        return ops->stat(node, st);

    st->type        = node->type;
    st->size        = node->size;
    st->permissions = node->permissions;
    st->inode       = node->inode;
    st->created     = 0;
    st->modified    = 0;
    return 0;
}
