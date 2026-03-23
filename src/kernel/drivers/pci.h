#pragma once

#include <aevos/types.h>

/* x86_64：与 vmm map_huge_range(vbase=PHYS_MAP_BASE+LO,pbase=LO) 一致 → VA=PHYS_MAP_BASE+phys */
#if defined(__x86_64__)
/*
 * QEMU i440FX + virtio modern：64-bit BAR 常见落在 0xC000000000。
 * 只映射 2MiB 大页以降低页表分配压力；若 BAR 变化请调整或扩大窗口。
 */
#define AEVOS_X86_PCI_MMIO64_PHYS_LO  0xC000000000ULL
#define AEVOS_X86_PCI_MMIO64_PHYS_SZ  (1ULL << 30) /* 1 GiB：覆盖整段 64-bit BAR */
#endif

#define PCI_MAX_DEVICES 256
#define PCI_MAX_BUS     256
#define PCI_MAX_DEV     32
#define PCI_MAX_FUNC    8

#define PCI_BAR_IO      0x01
#define PCI_BAR_MEM     0x00
#define PCI_BAR_MEM_64  0x04

#define PCI_VENDOR_NONE 0xFFFF

/* Standard PCI config space offsets */
#define PCI_CFG_VENDOR_ID    0x00
#define PCI_CFG_DEVICE_ID    0x02
#define PCI_CFG_COMMAND      0x04
#define PCI_CFG_STATUS       0x06
#define PCI_CFG_REVISION     0x08
#define PCI_CFG_PROG_IF      0x09
#define PCI_CFG_SUBCLASS     0x0A
#define PCI_CFG_CLASS        0x0B
#define PCI_CFG_HEADER_TYPE  0x0E
#define PCI_CFG_BAR0         0x10
#define PCI_CFG_BAR1         0x14
#define PCI_CFG_BAR2         0x18
#define PCI_CFG_BAR3         0x1C
#define PCI_CFG_BAR4         0x20
#define PCI_CFG_BAR5         0x24
#define PCI_CFG_INT_LINE     0x3C
#define PCI_CFG_INT_PIN      0x3D

typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  irq;
    uint64_t bar[6];
} pci_device_t;

void          pci_init(void);
uint32_t      pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void          pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);
uint8_t       pci_read_config8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
uint16_t      pci_read_config16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset);
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);
uint64_t      pci_get_bar(pci_device_t *dev, uint8_t bar_index);
uint32_t      pci_get_device_count(void);
pci_device_t *pci_get_device(uint32_t index);

/*
 * x86_64：PCI 内存 BAR 的 CPU 访问地址。低 4G 用身份映射；高 BAR 用 PHYS_MAP_BASE
 * 映射（需 vmm_init 已映射 AEVOS_X86_PCI_MMIO64_* 窗口）。其它架构多为已通过
 * decode_bar 得到的可直接访问地址。
 */
void *pci_bar_to_mmio_vaddr(uint64_t bar_phys);
