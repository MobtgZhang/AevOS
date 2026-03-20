#pragma once

#include <aevos/types.h>
#include "vfs.h"
#include <aevos/types.h>

#define AEVOSFS_MAGIC          0xAE05F5A1
#define AEVOSFS_VERSION        1
#define AEVOSFS_BLOCK_SIZE     4096
#define AEVOSFS_DIRECT_BLOCKS  16
#define AEVOSFS_MAX_NAME       128
#define AEVOSFS_MAX_INODES     4096
#define AEVOSFS_LOG_ENTRY_WRITE  1
#define AEVOSFS_LOG_ENTRY_DELETE 2

typedef ssize_t (*aevosfs_dev_read_fn)(uint64_t block, void *buf, size_t count);
typedef ssize_t (*aevosfs_dev_write_fn)(uint64_t block, const void *buf, size_t count);

typedef struct PACKED {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t root_inode;
    uint64_t log_head;
    uint64_t checkpoint_block;
    uint8_t  reserved[448];
} aevosfs_superblock_t;

typedef enum {
    AEVOSFS_FILE,
    AEVOSFS_DIR
} aevosfs_inode_type_t;

typedef struct PACKED {
    uint64_t             id;
    aevosfs_inode_type_t type;
    uint64_t             size;
    uint64_t             created;
    uint64_t             modified;
    uint64_t             block_list[AEVOSFS_DIRECT_BLOCKS];
    uint64_t             indirect_block;
    uint32_t             permissions;
    uint32_t             link_count;
    uint8_t              reserved[40];
} aevosfs_inode_t;

typedef struct PACKED {
    uint64_t             inode_id;
    char                 name[AEVOSFS_MAX_NAME];
    aevosfs_inode_type_t type;
} aevosfs_dir_entry_t;

typedef struct PACKED {
    uint8_t  entry_type;
    uint64_t inode_id;
    uint64_t offset;
    uint32_t length;
    uint64_t data_block;
    uint64_t timestamp;
} aevosfs_log_entry_t;

int      aevosfs_init(aevosfs_dev_read_fn read_fn, aevosfs_dev_write_fn write_fn);
int      aevosfs_mount(void);

int      aevosfs_create(const char *path, aevosfs_inode_type_t type);
ssize_t  aevosfs_read(aevosfs_inode_t *inode, uint64_t offset, void *buf, size_t size);
ssize_t  aevosfs_write(aevosfs_inode_t *inode, uint64_t offset, const void *buf, size_t size);
int      aevosfs_list_dir(aevosfs_inode_t *inode, aevosfs_dir_entry_t *entries, size_t max);
int      aevosfs_checkpoint(void);

vfs_ops_t *aevosfs_get_vfs_ops(void);
