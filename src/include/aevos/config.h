#pragma once

#include <aevos/version.h>

#define AEVOS_NAME "AevOS - Autonomous Evolving OS"

#define PAGE_SIZE          4096UL
#define KERNEL_STACK_SIZE  (64 * 1024)
#define USER_STACK_SIZE    (64 * 1024)

/* ── Architecture-specific memory layout ─────────────── */

#if defined(__x86_64__)
#define KERNEL_VBASE       0xFFFF800000000000ULL
#define KERNEL_HEAP_BASE   0xFFFF800040000000ULL
#define PHYS_MAP_BASE      0xFFFF800000000000ULL
#define FRAMEBUFFER_VBASE  0xFFFF900000000000ULL
#define USER_SPACE_TOP     0x00007FFFFFFFFFFFULL
#define USER_STACK_TOP     0x00007FFFFFF00000ULL

#elif defined(__aarch64__)
#define KERNEL_VBASE       0xFFFF000000000000ULL
#define KERNEL_HEAP_BASE   0xFFFF000040000000ULL
#define PHYS_MAP_BASE      0xFFFF000000000000ULL
#define FRAMEBUFFER_VBASE  0xFFFF100000000000ULL
#define USER_SPACE_TOP     0x0000FFFFFFFFFFFFULL
#define USER_STACK_TOP     0x0000FFFFFF00000ULL

#elif defined(__riscv)
/*
 * Sv48 规范：bit47 须与 bits63–48 符号扩展一致。0xFFFF000000000000 的 bit47=0 属非法 VA，
 * 取指即 EXCEPT_INST_ACCESS_PAGE_FAULT。须用 bit47=1 的区间（如 0xFFFF800000000000）。
 * 同时 PHYS_MAP_BASE + 0x4000000000 须在 uint64 内不溢出（0xFFFFFFC000000000 会回绕为 0）。
 */
#define KERNEL_VBASE       0xFFFF800000000000ULL
#define KERNEL_HEAP_BASE   0xFFFF800040000000ULL
#define PHYS_MAP_BASE      0xFFFF800000000000ULL
#define FRAMEBUFFER_VBASE  0xFFFF900000000000ULL
#define USER_SPACE_TOP     0x0000003FFFFFFFFFULL
#define USER_STACK_TOP     0x0000003FFF00000ULL

#elif defined(__loongarch64)
#define KERNEL_VBASE       0x9000000000000000ULL
#define KERNEL_HEAP_BASE   0x9000000040000000ULL
#define PHYS_MAP_BASE      0x9000000000000000ULL
#define FRAMEBUFFER_VBASE  0x9000100000000000ULL
#define USER_SPACE_TOP     0x00007FFFFFFFFFFFULL
#define USER_STACK_TOP     0x00007FFFFFF00000ULL

#else
#define KERNEL_VBASE       0xFFFF800000000000ULL
#define KERNEL_HEAP_BASE   0xFFFF800040000000ULL
#define PHYS_MAP_BASE      0xFFFF800000000000ULL
#define FRAMEBUFFER_VBASE  0xFFFF900000000000ULL
#define USER_SPACE_TOP     0x00007FFFFFFFFFFFULL
#define USER_STACK_TOP     0x00007FFFFFF00000ULL
#endif

#define KERNEL_HEAP_SIZE   (256 * 1024 * 1024ULL)

/* ── Resource limits ─────────────────────────────────── */

#define MAX_AGENTS         64
#define MAX_COROUTINES     1024
#define MAX_SKILLS         1024
#define MAX_OPEN_FILES     256

#define HIST_RING_SIZE     4096
#define EMBED_DIM          512
#define MEM_MAX_ENTRIES    65536

#define LLM_DEFAULT_CTX    32768
#define LLM_MAX_BATCH      512

#define TIMER_FREQ_HZ      1000

/* ── Architecture-specific hardware addresses ────────── */

#if defined(__x86_64__)
#define SERIAL_PORT        0x3F8
#define SERIAL_USE_PIO     1
#define PCI_CONFIG_ADDR    0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_USE_ECAM       0
#elif defined(__aarch64__)
#define SERIAL_PORT        0
#define SERIAL_MMIO_BASE   (PHYS_MAP_BASE + 0x09000000ULL)
#define SERIAL_USE_PIO     0
#define PCI_CONFIG_ADDR    0
#define PCI_CONFIG_DATA    0
#define AARCH64_MMIO_BASE  PHYS_MAP_BASE
/*
 * QEMU virt (highmem=on, default) places PCI ECAM at 0x4010000000.
 * 256 buses × 1 MiB each = 256 MiB ECAM window.
 */
#define PCI_ECAM_PHYS      0x4010000000ULL
#define PCI_ECAM_SIZE      0x10000000ULL
#define PCI_ECAM_BASE      (PHYS_MAP_BASE + PCI_ECAM_PHYS)
#define PCI_ECAM_BUS_LIMIT 16
#define PCI_USE_ECAM       1
/*
 * 64-bit PCI MMIO window: QEMU virt places 64-bit BARs starting at
 * physical 0x8000000000.  Map a 256 MiB slice to cover typical BARs.
 */
#define PCI_MMIO64_PHYS    0x8000000000ULL
#define PCI_MMIO64_SIZE    0x10000000ULL
#elif defined(__riscv)
/*
 * QEMU virt：16550 @ 0x10000000，PCIe ECAM @ 0x30000000（与 efi_main Sv48 线性映射一致）。
 * MMIO 通过 PHYS_MAP_BASE 访问，与 pci decode_bar、内核 vmm 直接映射一致。
 */
#define SERIAL_PORT        0
#define SERIAL_MMIO_BASE   (PHYS_MAP_BASE + 0x10000000ULL)
#define SERIAL_USE_PIO     0
#define PCI_CONFIG_ADDR    0
#define PCI_CONFIG_DATA    0
#define PCI_ECAM_PHYS      0x30000000ULL
#define PCI_ECAM_SIZE      0x10000000ULL
#define PCI_ECAM_BASE      (PHYS_MAP_BASE + PCI_ECAM_PHYS)
#define PCI_ECAM_BUS_LIMIT 16
#define PCI_USE_ECAM       1
/*
 * QEMU virt (RV64)：64-bit BAR 放在「DRAM 末端向上对齐 16GiB」之后的高 MMIO 窗
 *（见 qemu hw/riscv/virt.c virt_high_pcie_memmap）。`-m 4G` 时常见基址为 0x4000000000。
 * 若增大内存导致该窗平移，需同步调整或改为由 boot_info 传入。
 */
#define PCI_MMIO64_PHYS    0x4000000000ULL
/*
 * 须覆盖 QEMU hw/riscv/virt.c 中 virt_high_pcie_memmap 整段高 MMIO 孔洞
 *（VIRT64_HIGH_PCIE_MMIO_SIZE = 16 GiB）。仅映射 1 GiB 时，部分设备的 64-bit BAR
 * 会落在窗口更深处，内核首次写 virtio-gpu common_cfg 即缺页挂死，QEMU 显示
 * “Display output is not active”。
 */
#define PCI_MMIO64_SIZE    0x4000000000ULL
#elif defined(__loongarch64)
#define SERIAL_PORT        0
#define SERIAL_MMIO_BASE   0x1FE001E0ULL
#define SERIAL_USE_PIO     0
#define PCI_CONFIG_ADDR    0
#define PCI_CONFIG_DATA    0
#define LOONGARCH_MMIO_BASE PHYS_MAP_BASE
#define PCI_ECAM_BASE      (LOONGARCH_MMIO_BASE + 0x20000000ULL)
#define PCI_ECAM_BUS_LIMIT 128
#define PCI_USE_ECAM       1
#else
#define SERIAL_PORT        0
#define SERIAL_USE_PIO     0
#define PCI_CONFIG_ADDR    0
#define PCI_CONFIG_DATA    0
#define PCI_USE_ECAM       0
#endif

/* ── Allocator tunables ──────────────────────────────── */

#define SLAB_MIN_SIZE      32
/* Must be >= CORO_STACK_SIZE (coroutine stacks use kmalloc). */
#define SLAB_MAX_SIZE      (64 * 1024)

#define SKILL_SUCCESS_THRESHOLD  0.6f
#define SKILL_MIN_CALLS_EVAL     10
#define SKILL_AUTO_EVOLVE_COOLDOWN_MS 60000

/* 1=内核链接完整 GGUF 推理；0=仅桩 + llm_ipc 用户态服务路径（make AEVOS_EMBED_LLM=0） */
#ifndef AEVOS_EMBED_LLM
#define AEVOS_EMBED_LLM 1
#endif
