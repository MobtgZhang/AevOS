#include <posix/unistd.h>
#include <aevos/types.h>
#include "../lib/string.h"
#include "../kernel/fs/vfs.h"

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

static uint32_t oflag_to_vfs(int oflag)
{
    uint32_t vf  = 0;
    int      acc = oflag & O_ACCMODE;

    if (acc == O_RDONLY)
        vf |= VFS_O_READ;
    else if (acc == O_WRONLY)
        vf |= VFS_O_WRITE;
    else if (acc == O_RDWR)
        vf |= VFS_O_READ | VFS_O_WRITE;

    if (oflag & O_CREAT)
        vf |= VFS_O_CREATE;
    if (oflag & O_TRUNC)
        vf |= VFS_O_TRUNC;
    if (oflag & O_APPEND)
        vf |= VFS_O_APPEND;

    return vf;
}

int posix_open(const char *path, int oflag, ...)
{
    posix_seterr_from_kerr(0);

    if (!path) {
        errno = P_EINVAL;
        return -1;
    }

    if ((oflag & O_CREAT) && (oflag & O_EXCL)) {
        vfs_stat_t tst;
        if (vfs_stat(path, &tst) == 0) {
            errno = P_EEXIST;
            return -1;
        }
    }

    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        (void)va_arg(ap, unsigned int);
        va_end(ap);
    }

    vfs_fd_t fd = vfs_open(path, oflag_to_vfs(oflag));
    if (fd < 0) {
        posix_seterr_from_kerr((int)fd);
        return -1;
    }
    return (int)fd;
}

int posix_close(int fd)
{
    int rc = vfs_close((vfs_fd_t)fd);
    if (rc < 0) {
        posix_seterr_from_kerr(rc);
        return -1;
    }
    return 0;
}

ssize_t posix_read(int fd, void *buf, size_t count)
{
    ssize_t n = vfs_read((vfs_fd_t)fd, buf, count);
    if (n < 0) {
        posix_seterr_from_kerr((int)n);
        return -1;
    }
    return n;
}

ssize_t posix_write(int fd, const void *buf, size_t count)
{
    ssize_t n = vfs_write((vfs_fd_t)fd, buf, count);
    if (n < 0) {
        posix_seterr_from_kerr((int)n);
        return -1;
    }
    return n;
}

posix_off_t posix_lseek(int fd, posix_off_t offset, int whence)
{
    int64_t r = vfs_lseek((vfs_fd_t)fd, offset, whence);
    if (r < 0) {
        posix_seterr_from_kerr((int)r);
        return (posix_off_t)-1;
    }
    return (posix_off_t)r;
}

static void vfs_stat_to_posix(const vfs_stat_t *vs, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_dev     = 1;
    st->st_ino     = vs->inode;
    st->st_uid     = 0;
    st->st_gid     = 0;
    st->st_rdev    = 0;
    st->st_size    = (posix_off_t)vs->size;
    st->st_blksize = 512;
    if (vs->size > 0)
        st->st_blocks = (posix_blkcnt_t)((vs->size + 511) / 512);
    st->st_nlink = 1;

    st->st_mode = (posix_mode_t)(vs->permissions & 0777);
    if (vs->type == VFS_DIR)
        st->st_mode |= S_IFDIR;
    else if (vs->type == VFS_FILE)
        st->st_mode |= S_IFREG;
    else if (vs->type == VFS_SYMLINK)
        st->st_mode |= S_IFREG;
    else
        st->st_mode |= S_IFREG;
}

int posix_stat(const char *path, struct stat *buf)
{
    if (!path || !buf) {
        errno = P_EINVAL;
        return -1;
    }

    vfs_stat_t vs;
    int        rc = vfs_stat(path, &vs);
    if (rc < 0) {
        posix_seterr_from_kerr(rc);
        return -1;
    }
    vfs_stat_to_posix(&vs, buf);
    return 0;
}

int posix_fstat(int fd, struct stat *buf)
{
    if (!buf) {
        errno = P_EINVAL;
        return -1;
    }

    vfs_stat_t vs;
    int        rc = vfs_fstat((vfs_fd_t)fd, &vs);
    if (rc < 0) {
        posix_seterr_from_kerr(rc);
        return -1;
    }
    vfs_stat_to_posix(&vs, buf);
    return 0;
}

int posix_mkdir(const char *path, posix_mode_t mode)
{
    (void)mode;
    if (!path) {
        errno = P_EINVAL;
        return -1;
    }
    int rc = vfs_mkdir(path);
    if (rc < 0) {
        posix_seterr_from_kerr(rc);
        return -1;
    }
    return 0;
}
