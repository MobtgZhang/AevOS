#pragma once

#include <posix/sys/types.h>

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

struct stat {
    posix_dev_t     st_dev;
    posix_ino_t     st_ino;
    posix_mode_t    st_mode;
    posix_nlink_t   st_nlink;
    posix_uid_t     st_uid;
    posix_gid_t     st_gid;
    posix_dev_t     st_rdev;
    posix_off_t     st_size;
    posix_blksize_t st_blksize;
    posix_blkcnt_t  st_blocks;
    posix_time_t    st_atime;
    posix_time_t    st_mtime;
    posix_time_t    st_ctime;
};
