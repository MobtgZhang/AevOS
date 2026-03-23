#pragma once

#include <aevos/types.h>

/*
 * VirtIO 1.0 PCI：common configuration 仅允许 8/16/32 位访问（见规范
 * “Common configuration structure layout”）。对队列地址等 64 位字段须拆成
 * 两次 32 位写入，否则部分实现上 64 位 MMIO 会挂死或行为未定义。
 */
static inline void virtio_pci_common_cfg_write64(volatile uint8_t *cfg,
                                                 uint32_t off, uint64_t val)
{
    *(volatile uint32_t *)(cfg + off)     = (uint32_t)val;
    *(volatile uint32_t *)(cfg + off + 4) = (uint32_t)(val >> 32);
}
