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

typedef struct {
    uint64_t    base;
    uint64_t    length;
    mmap_type_t type;
} PACKED mmap_entry_t;

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

typedef struct {
    char     model_path[128];
    uint32_t n_ctx;
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
} PACKED boot_info_t;
