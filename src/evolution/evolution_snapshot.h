#pragma once

#include <aevos/types.h>

#define EV_SNAPSHOT_MAX_LABEL 64

typedef struct {
    uint32_t version;
    uint64_t kernel_elf_crc;
    char     label[EV_SNAPSHOT_MAX_LABEL];
    uint64_t created_tick;
} evolution_snapshot_meta_t;

void evolution_snapshot_init(void);

/* 注册当前运行版本元数据（ELF CRC 占位为 0）。 */
int evolution_snapshot_register_current(const char *label);

int evolution_snapshot_count(void);

/*
 * 应用快照 id 回滚（占位：仅记录日志；与 AevOSFS checkpoint 协同见 aevosfs）。 */
int evolution_snapshot_rollback(int snapshot_id);
