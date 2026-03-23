#pragma once

#include <aevos/types.h>

#define MMAP_MAX_ENTRIES 256
#define BOOT_MAGIC       0xAE05B007

typedef enum {
    MMAP_USABLE        = 0,
    MMAP_RESERVED      = 1,
    MMAP_ACPI_RECLAIM  = 2,
    MMAP_ACPI_NVS      = 3,
    MMAP_FRAMEBUFFER   = 4,
    MMAP_KERNEL        = 5,
    MMAP_BOOTLOADER    = 6,
} mmap_type_t;

/*
 * Must match src/boot/efi_main.c mmap_entry_t: UINT64, UINT64, UINT32.
 * Using uint32_t (not enum) avoids GCC packing enums to 1 byte in PACKED
 * structs, which shifted all following boot_info fields (fb, cfg, …) by
 * 768 bytes and broke framebuffer handoff on AArch64.
 */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type; /* mmap_type_t value */
} PACKED mmap_entry_t;

_Static_assert(sizeof(mmap_entry_t) == 20, "mmap_entry must match UEFI stub");

typedef struct {
    mmap_entry_t entries[MMAP_MAX_ENTRIES];
    uint32_t     count;
    uint64_t     key;
} uefi_mmap_t;

typedef struct {
    uint32_t *base;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    uint32_t  bpp;
} framebuffer_t;

/* Display / scheduler hints only — local LLM path is configured in UI (e.g. terminal: llm load). */
typedef struct {
    uint32_t n_threads;
    bool     use_gpu;
    uint32_t target_fps;
    uint32_t screen_width;
    uint32_t screen_height;
} aevos_boot_cfg_t;

typedef struct {
    uint64_t        magic;
    uefi_mmap_t     mmap;
    framebuffer_t   fb;
    aevos_boot_cfg_t cfg;
    uint64_t        rsdp;
    uint64_t        kernel_phys_base;
    uint64_t        kernel_size;
    uint64_t        total_memory;
    /*
     * L0 可观测性：固件内时间戳（x86=RDTSC，aarch64=cntvct_el0，riscv=rdtime）。
     * 用于衡量「UEFI 阶段耗时」；与 ideas2 中 <2s 到推理就绪 的目标对齐。
     */
    uint64_t        uefi_boot_cycle_start;
    uint64_t        uefi_boot_cycle_handoff;
} PACKED boot_info_t;
