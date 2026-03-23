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
#define KERNEL_VBASE       0xFFFFFFC000000000ULL
#define KERNEL_HEAP_BASE   0xFFFFFFC040000000ULL
#define PHYS_MAP_BASE      0xFFFFFFC000000000ULL
#define FRAMEBUFFER_VBASE  0xFFFFFFD000000000ULL
#define USER_SPACE_TOP     0x0000003FFFFFFFFFULL
#define USER_STACK_TOP     0x0000003FFF00000ULL

#elif defined(__loongarch64)
#define KERNEL_VBASE       0x9000000000000000ULL
#define KERNEL_HEAP_BASE   0x9000000040000000ULL
#define PHYS_MAP_BASE      0x9000000000000000ULL
#define FRAMEBUFFER_VBASE  0x9000100000000000ULL
#define USER_SPACE_TOP     0x00007FFFFFFFFFFFULL
#define USER_STACK_TOP     0x00007FFFFFF00000ULL

#elif defined(__mips64)
/*
 * kseg0 (cached, no TLB): 0xFFFFFFFF80000000 + phys
 * kseg1 (uncached, no TLB): 0xFFFFFFFFA0000000 + phys
 * Covers physical 0x00000000–0x1FFFFFFF — enough for QEMU Malta 256 MB.
 */
#define KERNEL_VBASE       0xFFFFFFFF80000000ULL
#define KERNEL_HEAP_BASE   0xFFFFFFFF84000000ULL
#define PHYS_MAP_BASE      0xFFFFFFFF80000000ULL
#define FRAMEBUFFER_VBASE  0xFFFFFFFFA0000000ULL
#define USER_SPACE_TOP     0x000000007FFFFFFFULL
#define USER_STACK_TOP     0x000000007FF00000ULL
/* kseg1: uncached direct-mapped segment for MMIO */
#define MIPS64_UNCACHED_BASE 0xFFFFFFFFA0000000ULL

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
#elif defined(__mips64)
/*
 * QEMU Malta: 16550 UART at ISA I/O address 0x3F8, mapped to
 * physical 0x180003F8.  Accessed via xkphys uncached segment.
 */
#define SERIAL_PORT        0
#define SERIAL_MMIO_BASE   (MIPS64_UNCACHED_BASE + 0x180003F8ULL)
#define SERIAL_USE_PIO     0
#define PCI_CONFIG_ADDR    0
#define PCI_CONFIG_DATA    0
#define MIPS64_MMIO_BASE   MIPS64_UNCACHED_BASE
#define PCI_ECAM_BASE      (MIPS64_UNCACHED_BASE + 0x1BE00000ULL)
#define PCI_ECAM_BUS_LIMIT 16
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
