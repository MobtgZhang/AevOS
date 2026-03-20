/*
 * AevOS UEFI Boot Stub — Multi-architecture
 *
 * Responsibilities:
 *   1. Initialize GOP framebuffer
 *   2. Load kernel.elf from ESP, parse ELF segments
 *   3. Set up page tables (identity + higher-half)
 *   4. Obtain UEFI memory map
 *   5. Fill boot_info_t
 *   6. ExitBootServices, load CR3, and jump to kernel
 */

#include "efi_types.h"
#include <aevos/version.h>

#define BOOT_MAGIC       0xAE05B007
#define MMAP_MAX_ENTRIES 256

#define KERNEL_VBASE_X86   0xFFFF800000000000ULL

#define PT_LOAD 1

#define PTE_PRESENT   0x01ULL
#define PTE_WRITE     0x02ULL
#define PTE_PS        0x80ULL  /* 2 MB large page */

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
    UINT64     base;
    UINT64     length;
    UINT32     type;
} __attribute__((packed)) mmap_entry_t;

typedef struct {
    mmap_entry_t entries[MMAP_MAX_ENTRIES];
    UINT32       count;
    UINT64       key;
} uefi_mmap_t;

typedef struct {
    UINT32 *base;
    UINT32  width;
    UINT32  height;
    UINT32  pitch;
    UINT32  bpp;
} framebuffer_t;

typedef struct {
    char     model_path[128];
    UINT32   n_ctx;
    UINT32   n_threads;
    UINT8    use_gpu;
    UINT32   target_fps;
    UINT32   screen_width;
    UINT32   screen_height;
} aevos_boot_cfg_t;

typedef struct {
    UINT64          magic;
    uefi_mmap_t     mmap;
    framebuffer_t   fb;
    aevos_boot_cfg_t cfg;
    UINT64          rsdp;
    UINT64          kernel_phys_base;
    UINT64          kernel_size;
    UINT64          total_memory;
} __attribute__((packed)) boot_info_t;

/* ── ELF64 structures ────────────────────────────────── */

typedef struct {
    UINT8   e_ident[16];
    UINT16  e_type;
    UINT16  e_machine;
    UINT32  e_version;
    UINT64  e_entry;
    UINT64  e_phoff;
    UINT64  e_shoff;
    UINT32  e_flags;
    UINT16  e_ehsize;
    UINT16  e_phentsize;
    UINT16  e_phnum;
    UINT16  e_shentsize;
    UINT16  e_shnum;
    UINT16  e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32  p_type;
    UINT32  p_flags;
    UINT64  p_offset;
    UINT64  p_vaddr;
    UINT64  p_paddr;
    UINT64  p_filesz;
    UINT64  p_memsz;
    UINT64  p_align;
} Elf64_Phdr;

/* ── Minimal helpers ──────────────────────────────────── */

static void *boot_memset(void *s, int c, UINTN n) {
    UINT8 *p = (UINT8 *)s;
    while (n--) *p++ = (UINT8)c;
    return s;
}

static void *boot_memcpy(void *dst, const void *src, UINTN n) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ── Console output ───────────────────────────────────── */

static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *gConOut;

static void print(const CHAR16 *s) {
    if (gConOut) gConOut->OutputString(gConOut, (CHAR16 *)s);
}

static void print_hex(UINT64 val) {
    CHAR16 buf[19];
    buf[0] = u'0'; buf[1] = u'x';
    for (int i = 15; i >= 0; i--) {
        UINT8 nib = (val >> (i * 4)) & 0xF;
        buf[17 - i] = (nib < 10) ? (u'0' + nib) : (u'A' + nib - 10);
    }
    buf[18] = 0;
    print(buf);
}

static void print_dec(UINT64 val) {
    CHAR16 buf[21];
    int i = 20;
    buf[i] = 0;
    if (val == 0) { buf[--i] = u'0'; }
    else while (val > 0) { buf[--i] = u'0' + (CHAR16)(val % 10); val /= 10; }
    print(buf + i);
}

/* ── GUID comparison ──────────────────────────────────── */

static BOOLEAN guid_eq(EFI_GUID *a, EFI_GUID *b) {
    UINT8 *pa = (UINT8 *)a, *pb = (UINT8 *)b;
    for (int i = 0; i < 16; i++)
        if (pa[i] != pb[i]) return 0;
    return 1;
}

/* ── GOP initialization ───────────────────────────────── */

static EFI_STATUS init_gop(EFI_BOOT_SERVICES *bs, framebuffer_t *fb) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;

    EFI_STATUS status = bs->LocateProtocol(&gop_guid, 0, (VOID **)&gop);
    if (status != EFI_SUCCESS || !gop) {
        print(u"GOP not found\r\n");
        return status;
    }

    UINT32 best_mode = gop->Mode->Mode;
    UINT32 best_w = 0, best_h = 0;

    for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        UINTN info_sz;
        if (gop->QueryMode(gop, i, &info_sz, &info) != EFI_SUCCESS)
            continue;
        UINT32 w = info->HorizontalResolution;
        UINT32 h = info->VerticalResolution;
        if (w == 1280 && h == 800) { best_mode = i; best_w = w; best_h = h; break; }
        if (w * h > best_w * best_h) { best_mode = i; best_w = w; best_h = h; }
    }

    if (best_mode != gop->Mode->Mode)
        gop->SetMode(gop, best_mode);

    fb->base   = (UINT32 *)(UINTN)gop->Mode->FrameBufferBase;
    fb->width  = gop->Mode->Info->HorizontalResolution;
    fb->height = gop->Mode->Info->VerticalResolution;
    fb->pitch  = gop->Mode->Info->PixelsPerScanLine * 4;
    fb->bpp    = 32;

    return EFI_SUCCESS;
}

/* ── Read kernel ELF file from ESP ───────────────────── */

static EFI_STATUS read_kernel_file(EFI_BOOT_SERVICES *bs, EFI_HANDLE image_handle,
                                    UINT8 **out_buf, UINTN *out_size) {
    EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid  = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    VOID *lip_raw = 0;
    EFI_STATUS status;

    typedef struct {
        UINT32 Revision;
        EFI_HANDLE ParentHandle;
        VOID *SystemTable;
        EFI_HANDLE DeviceHandle;
        VOID *FilePath;
        VOID *Reserved;
        UINT32 LoadOptionsSize;
        VOID *LoadOptions;
        VOID *ImageBase;
        UINT64 ImageSize;
        EFI_MEMORY_TYPE ImageCodeType;
        EFI_MEMORY_TYPE ImageDataType;
        VOID *Unload;
    } EFI_LOADED_IMAGE;

    status = bs->HandleProtocol(image_handle, &lip_guid, &lip_raw);
    if (status != EFI_SUCCESS) return status;

    EFI_LOADED_IMAGE *li = (EFI_LOADED_IMAGE *)lip_raw;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    status = bs->HandleProtocol(li->DeviceHandle, &fs_guid, (VOID **)&fs);
    if (status != EFI_SUCCESS) return status;

    EFI_FILE_PROTOCOL *root = 0;
    status = fs->OpenVolume(fs, &root);
    if (status != EFI_SUCCESS) return status;

    EFI_FILE_PROTOCOL *kfile = 0;
    CHAR16 kpath[] = u"\\EFI\\AevOS\\kernel.elf";
    status = root->Open(root, &kfile, kpath, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        print(u"Cannot open kernel.elf\r\n");
        return status;
    }

    #define KERNEL_MAX_SIZE (32ULL * 1024 * 1024)
    UINTN pages = (KERNEL_MAX_SIZE + 4095) / 4096;

    EFI_PHYSICAL_ADDRESS buf = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &buf);
    if (status != EFI_SUCCESS) return status;

    UINTN read_sz = KERNEL_MAX_SIZE;
    status = kfile->Read(kfile, &read_sz, (VOID *)(UINTN)buf);
    kfile->Close(kfile);
    root->Close(root);

    if (status != EFI_SUCCESS) {
        bs->FreePages(buf, pages);
        return status;
    }

    *out_buf  = (UINT8 *)(UINTN)buf;
    *out_size = read_sz;
    return EFI_SUCCESS;
}

/* ── Load ELF segments to their physical addresses ───── */

static EFI_STATUS load_elf_segments(EFI_BOOT_SERVICES *bs,
                                     UINT8 *elf_data, UINTN elf_size,
                                     UINT64 *entry_point,
                                     UINT64 *kernel_phys_lo,
                                     UINT64 *kernel_phys_hi) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;

    if (elf_size < sizeof(Elf64_Ehdr)) return EFI_LOAD_ERROR;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F')
        return EFI_LOAD_ERROR;

    *entry_point = ehdr->e_entry;
    *kernel_phys_lo = ~0ULL;
    *kernel_phys_hi = 0;

    for (UINT16 i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(elf_data + ehdr->e_phoff +
                                          (UINTN)i * ehdr->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;

        UINT64 paddr = ph->p_paddr;
        UINTN  pages = (UINTN)((ph->p_memsz + 4095) / 4096);

        /* Track physical range for boot_info */
        if (paddr < *kernel_phys_lo) *kernel_phys_lo = paddr;
        if (paddr + ph->p_memsz > *kernel_phys_hi)
            *kernel_phys_hi = paddr + ph->p_memsz;

        /* Allocate at fixed physical address */
        EFI_PHYSICAL_ADDRESS alloc_addr = paddr;
        EFI_STATUS status = bs->AllocatePages(AllocateAddress, EfiLoaderData,
                                               pages, &alloc_addr);
        if (status != EFI_SUCCESS) {
            print(u"  [ELF] AllocateAddress failed at ");
            print_hex(paddr);
            print(u", trying AnyPages\r\n");
            /* Fallback: this won't boot but let's continue to see what happens */
            status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                        pages, &alloc_addr);
            if (status != EFI_SUCCESS) return status;
        }

        boot_memset((void *)(UINTN)alloc_addr, 0, pages * 4096);

        if (ph->p_filesz > 0 && ph->p_offset + ph->p_filesz <= elf_size) {
            boot_memcpy((void *)(UINTN)alloc_addr,
                        elf_data + ph->p_offset,
                        (UINTN)ph->p_filesz);
        }

        print(u"  [ELF] LOAD vaddr=");
        print_hex(ph->p_vaddr);
        print(u" paddr=");
        print_hex(paddr);
        print(u" memsz=");
        print_dec(ph->p_memsz);
        print(u"\r\n");
    }

    return EFI_SUCCESS;
}

/* ── Set up x86_64 page tables (identity + higher-half) ─ */

#if defined(__x86_64__)

#define PT_PAGES 6  /* 1 PML4 + 1 PDPT + 4 PD */

static EFI_STATUS setup_page_tables(EFI_BOOT_SERVICES *bs,
                                     UINT64 *pt_base_out) {
    EFI_PHYSICAL_ADDRESS pt_base = 0;
    EFI_STATUS status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                           PT_PAGES, &pt_base);
    if (status != EFI_SUCCESS) return status;

    boot_memset((void *)(UINTN)pt_base, 0, PT_PAGES * 4096);

    UINT64 *pml4 = (UINT64 *)(UINTN)(pt_base);
    UINT64 *pdpt = (UINT64 *)(UINTN)(pt_base + 0x1000);
    UINT64 *pd0  = (UINT64 *)(UINTN)(pt_base + 0x2000);
    UINT64 *pd1  = (UINT64 *)(UINTN)(pt_base + 0x3000);
    UINT64 *pd2  = (UINT64 *)(UINTN)(pt_base + 0x4000);
    UINT64 *pd3  = (UINT64 *)(UINTN)(pt_base + 0x5000);

    /* PML4[0] and PML4[256] both point to PDPT */
    pml4[0]   = (UINT64)(UINTN)pdpt | PTE_PRESENT | PTE_WRITE;
    pml4[256] = (UINT64)(UINTN)pdpt | PTE_PRESENT | PTE_WRITE;

    /* PDPT entries pointing to 4 page directories (4 GB total) */
    pdpt[0] = (UINT64)(UINTN)pd0 | PTE_PRESENT | PTE_WRITE;
    pdpt[1] = (UINT64)(UINTN)pd1 | PTE_PRESENT | PTE_WRITE;
    pdpt[2] = (UINT64)(UINTN)pd2 | PTE_PRESENT | PTE_WRITE;
    pdpt[3] = (UINT64)(UINTN)pd3 | PTE_PRESENT | PTE_WRITE;

    /* Fill PD entries: each maps a 2 MB page */
    UINT64 *pds[4] = { pd0, pd1, pd2, pd3 };
    for (int g = 0; g < 4; g++) {
        for (int i = 0; i < 512; i++) {
            UINT64 phys = ((UINT64)g * 512 + (UINT64)i) * 0x200000ULL;
            pds[g][i] = phys | PTE_PRESENT | PTE_WRITE | PTE_PS;
        }
    }

    *pt_base_out = pt_base;
    return EFI_SUCCESS;
}

#endif /* __x86_64__ */

/* ── Set up aarch64 page tables (TTBR0 identity + TTBR1 higher-half) ── */

#if defined(__aarch64__)

/*
 * Map 8 GiB so that all of QEMU virt RAM is reachable even when UEFI
 * allocates boot_info or page tables above the 4 GiB boundary (common
 * with -m 4G where RAM spans physical 1–5 GiB).
 *
 * Layout per direction: 1 L0 + 1 L1 + 8 L2 = 10 pages × 2 = 20 pages.
 */
#define AARCH64_MAP_GIB   8
#define AARCH64_PT_PAGES  (2 * (1 + 1 + AARCH64_MAP_GIB))  /* 20 */

#define ARM64_VALID         (1ULL)
#define ARM64_TABLE         (1ULL << 1)
#define ARM64_TABLE_DESC    (ARM64_VALID | ARM64_TABLE)
#define ARM64_ATTR_IDX(n)   ((UINT64)(n) << 2)
#define ARM64_SH_INNER      (3ULL << 8)
#define ARM64_AF             (1ULL << 10)
#define ARM64_BLOCK_NORMAL  (ARM64_VALID | ARM64_ATTR_IDX(2) | \
                             ARM64_SH_INNER | ARM64_AF)
#define ARM64_BLOCK_DEVICE  (ARM64_VALID | ARM64_ATTR_IDX(0) | \
                             ARM64_SH_INNER | ARM64_AF)

/* QEMU virt aarch64: RAM starts at 1 GiB; everything below is device I/O */
#define AARCH64_DEVICE_TOP  0x40000000ULL

static EFI_STATUS setup_aarch64_page_tables(EFI_BOOT_SERVICES *bs,
                                             UINT64 *ttbr0_out,
                                             UINT64 *ttbr1_out)
{
    EFI_PHYSICAL_ADDRESS pt_base = 0;
    EFI_STATUS status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                           AARCH64_PT_PAGES, &pt_base);
    if (status != EFI_SUCCESS) return status;

    boot_memset((void *)(UINTN)pt_base, 0, AARCH64_PT_PAGES * 4096);

    int page_idx = 0;

    /* TTBR0 tables (identity map) */
    UINT64 *l0_id = (UINT64 *)(UINTN)(pt_base + page_idx++ * 0x1000);
    UINT64 *l1_id = (UINT64 *)(UINTN)(pt_base + page_idx++ * 0x1000);
    UINT64 *l2_id[AARCH64_MAP_GIB];
    for (int i = 0; i < AARCH64_MAP_GIB; i++)
        l2_id[i] = (UINT64 *)(UINTN)(pt_base + page_idx++ * 0x1000);

    /* TTBR1 tables (higher-half map) */
    UINT64 *l0_hi = (UINT64 *)(UINTN)(pt_base + page_idx++ * 0x1000);
    UINT64 *l1_hi = (UINT64 *)(UINTN)(pt_base + page_idx++ * 0x1000);
    UINT64 *l2_hi[AARCH64_MAP_GIB];
    for (int i = 0; i < AARCH64_MAP_GIB; i++)
        l2_hi[i] = (UINT64 *)(UINTN)(pt_base + page_idx++ * 0x1000);

    /* TTBR0 L0[0] → L1, L1[0..N-1] → L2 tables */
    l0_id[0] = (UINT64)(UINTN)l1_id | ARM64_TABLE_DESC;
    for (int i = 0; i < AARCH64_MAP_GIB; i++)
        l1_id[i] = (UINT64)(UINTN)l2_id[i] | ARM64_TABLE_DESC;

    /* Identity map: 8 GiB using 2 MB block descriptors.
     * Addresses below AARCH64_DEVICE_TOP (1 GiB) are PCI/GIC/UART
     * MMIO and must use Device-nGnRnE attributes.  Above that is RAM. */
    for (int g = 0; g < AARCH64_MAP_GIB; g++)
        for (int i = 0; i < 512; i++) {
            UINT64 phys = ((UINT64)g * 512 + (UINT64)i) * 0x200000ULL;
            l2_id[g][i] = phys | (phys < AARCH64_DEVICE_TOP
                                  ? ARM64_BLOCK_DEVICE : ARM64_BLOCK_NORMAL);
        }

    /* TTBR1 L0[0] → L1, L1[0..N-1] → L2 tables */
    l0_hi[0] = (UINT64)(UINTN)l1_hi | ARM64_TABLE_DESC;
    for (int i = 0; i < AARCH64_MAP_GIB; i++)
        l1_hi[i] = (UINT64)(UINTN)l2_hi[i] | ARM64_TABLE_DESC;

    /* Higher-half: same 8 GiB physical → KERNEL_VBASE + phys */
    for (int g = 0; g < AARCH64_MAP_GIB; g++)
        for (int i = 0; i < 512; i++) {
            UINT64 phys = ((UINT64)g * 512 + (UINT64)i) * 0x200000ULL;
            l2_hi[g][i] = phys | (phys < AARCH64_DEVICE_TOP
                                  ? ARM64_BLOCK_DEVICE : ARM64_BLOCK_NORMAL);
        }

    *ttbr0_out = (UINT64)(UINTN)l0_id;
    *ttbr1_out = (UINT64)(UINTN)l0_hi;
    return EFI_SUCCESS;
}

#endif /* __aarch64__ */

/* ── Memory map conversion ────────────────────────────── */

static mmap_type_t efi_to_aevos_memtype(UINT32 efi_type) {
    switch (efi_type) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiLoaderCode:
    case EfiLoaderData:
        return MMAP_USABLE;
    case EfiACPIReclaimMemory:
        return MMAP_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:
        return MMAP_ACPI_NVS;
    default:
        return MMAP_RESERVED;
    }
}

/* ── Find ACPI RSDP ───────────────────────────────────── */

static UINT64 find_rsdp(EFI_SYSTEM_TABLE *st) {
    EFI_GUID acpi_guid = ACPI_20_TABLE_GUID;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(&st->ConfigurationTable[i].VendorGuid, &acpi_guid))
            return (UINT64)(UINTN)st->ConfigurationTable[i].VendorTable;
    }
    return 0;
}

/* ── EFI Entry Point ──────────────────────────────────── */

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES *bs = st->BootServices;
    gConOut = st->ConOut;

    if (gConOut) gConOut->ClearScreen(gConOut);

    print(u"AevOS UEFI boot v");
    {
        const char *ver = AEVOS_VERSION_STRING;
        while (*ver) {
            CHAR16 wc[2] = { (CHAR16)(UINT8)(unsigned char)*ver++, 0 };
            print(wc);
        }
    }
    print(u"\r\n");
    print(u"===========================\r\n\r\n");

    /* Allocate boot_info in loader data (survives ExitBootServices) */
    EFI_PHYSICAL_ADDRESS bi_phys = 0;
    UINTN bi_pages = (sizeof(boot_info_t) + 4095) / 4096;
    EFI_STATUS status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                           bi_pages, &bi_phys);
    if (status != EFI_SUCCESS) {
        print(u"FATAL: cannot allocate boot_info page\r\n");
        for (;;);
    }
    boot_info_t *bi = (boot_info_t *)(UINTN)bi_phys;
    boot_memset(bi, 0, sizeof(boot_info_t));

    /* 1. GOP framebuffer (aligned local; bi is packed — do not pass &bi->fb) */
    framebuffer_t fb_work;
    boot_memset(&fb_work, 0, sizeof(fb_work));
    print(u"[gop] Initializing framebuffer...\r\n");
    status = init_gop(bs, &fb_work);
    if (status == EFI_SUCCESS) {
        boot_memcpy(&bi->fb, &fb_work, sizeof(framebuffer_t));
        print(u"[gop] Framebuffer: ");
        print_dec(fb_work.width);
        print(u"x");
        print_dec(fb_work.height);
        print(u"\r\n");
    }

    /* 2. Read kernel ELF file */
    print(u"[load] Reading kernel.elf from ESP...\r\n");
    UINT8 *elf_buf = 0;
    UINTN elf_size = 0;
    status = read_kernel_file(bs, image_handle, &elf_buf, &elf_size);
    if (status != EFI_SUCCESS) {
        print(u"FATAL: cannot read kernel\r\n");
        for (;;);
    }
    print(u"[load] Kernel file: ");
    print_dec(elf_size);
    print(u" bytes\r\n");

    /* 3. Parse ELF and load segments at physical addresses */
    print(u"[elf] Loading ELF segments...\r\n");
    UINT64 entry_point = 0;
    UINT64 kphys_lo = 0, kphys_hi = 0;
    status = load_elf_segments(bs, elf_buf, elf_size,
                                &entry_point, &kphys_lo, &kphys_hi);
    if (status != EFI_SUCCESS) {
        print(u"FATAL: cannot load kernel ELF\r\n");
        for (;;);
    }
    bi->kernel_phys_base = kphys_lo;
    bi->kernel_size      = kphys_hi - kphys_lo;

    print(u"[elf] Entry: ");
    print_hex(entry_point);
    print(u"\r\n");

    /* 4. Default boot config */
    const char model[] = "/models/default.gguf";
    boot_memcpy(bi->cfg.model_path, model, sizeof(model));
    bi->cfg.n_ctx         = 32768;
    bi->cfg.n_threads     = 4;
    bi->cfg.use_gpu       = 0;
    bi->cfg.target_fps    = 60;
    bi->cfg.screen_width  = fb_work.width;
    bi->cfg.screen_height = fb_work.height;

    /* 5. ACPI RSDP */
    bi->rsdp = find_rsdp(st);

    /* 6. Set up page tables (before ExitBootServices) */
#if defined(__x86_64__)
    print(u"[pt] Setting up page tables...\r\n");
    UINT64 pt_base = 0;
    status = setup_page_tables(bs, &pt_base);
    if (status != EFI_SUCCESS) {
        print(u"FATAL: cannot set up page tables\r\n");
        for (;;);
    }
    print(u"[pt] PML4 at ");
    print_hex(pt_base);
    print(u"\r\n");
#elif defined(__aarch64__)
    print(u"[pt] Setting up aarch64 page tables...\r\n");
    UINT64 ttbr0 = 0, ttbr1 = 0;
    status = setup_aarch64_page_tables(bs, &ttbr0, &ttbr1);
    if (status != EFI_SUCCESS) {
        print(u"FATAL: cannot set up page tables\r\n");
        for (;;);
    }
    print(u"[pt] TTBR0=");
    print_hex(ttbr0);
    print(u" TTBR1=");
    print_hex(ttbr1);
    print(u"\r\n");
#endif

    /* 7. Get memory map and ExitBootServices */
    print(u"[mem] Obtaining memory map...\r\n");

    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    UINT8 *mmap_buf = 0;

    bs->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
    map_size += 4096;

    status = bs->AllocatePool(EfiLoaderData, map_size, (VOID **)&mmap_buf);
    if (status != EFI_SUCCESS) {
        print(u"FATAL: cannot allocate memory map buffer\r\n");
        for (;;);
    }

    status = bs->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf,
                               &map_key, &desc_size, &desc_ver);
    if (status != EFI_SUCCESS) {
        print(u"FATAL: GetMemoryMap failed\r\n");
        for (;;);
    }

    /* Convert UEFI memory map */
    UINTN n_entries = map_size / desc_size;
    UINT64 total_mem = 0;
    bi->mmap.count = 0;

    for (UINTN i = 0; i < n_entries && bi->mmap.count < MMAP_MAX_ENTRIES; i++) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)(mmap_buf + i * desc_size);

        mmap_entry_t *e = &bi->mmap.entries[bi->mmap.count];
        e->base   = d->PhysicalStart;
        e->length = d->NumberOfPages * 4096ULL;
        e->type   = efi_to_aevos_memtype(d->Type);
        bi->mmap.count++;

        UINT64 end = d->PhysicalStart + d->NumberOfPages * 4096ULL;
        if (end > total_mem) total_mem = end;
    }
    bi->total_memory = total_mem;
    bi->magic = BOOT_MAGIC;

    /* Mark kernel region */
    for (UINT32 i = 0; i < bi->mmap.count; i++) {
        mmap_entry_t *e = &bi->mmap.entries[i];
        if (e->base <= kphys_lo &&
            kphys_hi <= e->base + e->length) {
            e->type = MMAP_KERNEL;
            break;
        }
    }

    print(u"[boot] ExitBootServices...\r\n");

    for (int retry = 0; retry < 4; retry++) {
        status = bs->ExitBootServices(image_handle, map_key);
        if (status == EFI_SUCCESS) break;
        map_size = 0;
        bs->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);
        map_size += 4096;
        bs->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf,
                          &map_key, &desc_size, &desc_ver);
    }

    if (status != EFI_SUCCESS) {
        for (;;);
    }

    /* === No UEFI services available beyond this point === */

#if defined(__x86_64__)
    /* Load our page tables (identity + higher-half) */
    __asm__ volatile("mov %0, %%cr3" : : "r"(pt_base) : "memory");

#elif defined(__aarch64__)
    {
        /*
         * AArch64 MMU reconfiguration.
         *
         * UEFI already had the MMU enabled.  We replace the translation
         * tables with our own that provide:
         *   TTBR0_EL1 → identity map  (0x0000_xxxx_xxxx_xxxx)
         *   TTBR1_EL1 → higher-half   (0xFFFF_xxxx_xxxx_xxxx)
         *
         * Steps: disable MMU → configure MAIR/TCR/TTBRs → re-enable.
         */
        UINT64 sctlr;
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        sctlr &= ~(1ULL << 0);  /* M: disable MMU */
        __asm__ volatile(
            "msr sctlr_el1, %0\n\t"
            "isb"
            : : "r"(sctlr) : "memory"
        );

        /* MAIR_EL1: Attr0=Device-nGnRnE, Attr1=Normal NC, Attr2=Normal WB */
        UINT64 mair = (0x00ULL)        |
                      (0x44ULL << 8)   |
                      (0xFFULL << 16);
        __asm__ volatile("msr mair_el1, %0" : : "r"(mair));

        /*
         * TCR_EL1:
         *   T0SZ=16  (48-bit VA for TTBR0)   T1SZ=16  (48-bit VA for TTBR1)
         *   TG0=4KB  TG1=4KB
         *   IPS=48-bit physical address space
         *   Inner-shareable, Write-back cacheable walks
         */
        UINT64 tcr = (16ULL << 0)  |   /* T0SZ  */
                     (1ULL  << 8)  |   /* IRGN0 */
                     (1ULL  << 10) |   /* ORGN0 */
                     (3ULL  << 12) |   /* SH0   */
                     (16ULL << 16) |   /* T1SZ  */
                     (1ULL  << 24) |   /* IRGN1 */
                     (1ULL  << 26) |   /* ORGN1 */
                     (3ULL  << 28) |   /* SH1   */
                     (2ULL  << 30) |   /* TG1=4KB */
                     (5ULL  << 32);    /* IPS=48-bit */
        __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr));

        /* Install page tables */
        __asm__ volatile("msr ttbr0_el1, %0" : : "r"(ttbr0));
        __asm__ volatile("msr ttbr1_el1, %0" : : "r"(ttbr1));

        /* Full TLB invalidate + barriers */
        __asm__ volatile(
            "isb\n\t"
            "tlbi vmalle1\n\t"
            "dsb sy\n\t"
            "isb"
        );

        /* Re-enable MMU, D-cache, I-cache */
        __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
        __asm__ volatile(
            "msr sctlr_el1, %0\n\t"
            "isb"
            : : "r"(sctlr) : "memory"
        );
    }
#endif

#if defined(__mips64) || defined(__mips__)
    {
        /*
         * MIPS64 UEFI path: ensure 64-bit kernel mode (Status.KX=1) and
         * convert the stack pointer to kseg0 so the kernel can use it
         * without TLB translations.
         */
        UINT32 sr;
        __asm__ volatile("mfc0 %0, $12" : "=r"(sr));
        sr |= (1u << 7) | (1u << 6) | (1u << 5); /* KX, SX, UX */
        sr &= ~((1u << 22) | (1u << 2));          /* clear BEV, ERL */
        __asm__ volatile("mtc0 %0, $12\n\t.set push\n\t.set noreorder\n\tnop\n\tnop\n\t.set pop" : : "r"(sr));

        UINT64 sp_val;
        __asm__ volatile("move %0, $sp" : "=r"(sp_val));
        /* Convert low physical SP to kseg0 address */
        if (sp_val < 0x20000000ULL) {
            sp_val |= 0xFFFFFFFF80000000ULL;
            __asm__ volatile("move $sp, %0" : : "r"(sp_val));
        }
    }
#endif

#if defined(__loongarch64) || defined(__loongarch__)
    {
        /*
         * LoongArch DMW (Direct Mapping Window) setup.
         * After ExitBootServices we are in DA (Direct Address) mode.
         * The kernel entry point is a DMW virtual address (0x9000...).
         * We configure DMW then switch to Mapped (PG) mode so the
         * kernel address is reachable.
         *
         * DMW0: VSEG=0x0 identity map, uncached, PLV0
         *       — covers current bootloader code so mode-switch is safe
         * DMW1: VSEG=0x9 cached, PLV0
         *       — covers kernel virtual addresses
         */
        UINT64 tmp;
        tmp = 0x0000000000000001ULL;
        __asm__ volatile("csrwr %0, 0x180" : "+r"(tmp));
        tmp = 0x9000000000000011ULL;
        __asm__ volatile("csrwr %0, 0x181" : "+r"(tmp));

        __asm__ volatile("csrrd %0, 0x0" : "=r"(tmp));
        tmp = (tmp & ~(1ULL << 3)) | (1ULL << 4);
        __asm__ volatile("csrwr %0, 0x0" : "+r"(tmp) :: "memory");
    }
#endif

    /* Jump to kernel entry (now accessible through higher-half mapping) */
    typedef void (*kernel_entry_t)(boot_info_t *);
    kernel_entry_t entry = (kernel_entry_t)(UINTN)entry_point;
    entry(bi);

    for (;;);
}
