#include <aevos/types.h>
#include <aevos/config.h>
#include <aevos/boot_info.h>
#include "klog.h"
#include "arch/arch.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/slab.h"
#include "drivers/serial.h"
#include "drivers/timer.h"
#include "drivers/gpu_fb.h"
#include "drivers/hid.h"
#include "drivers/pci.h"
#include "drivers/nvme.h"
#include "drivers/block_dev.h"
#include "drivers/ahci.h"
#include "drivers/audio.h"
#include "drivers/virtio_gpu.h"
#include "locale.h"
#include "fs/vfs.h"
#include "fs/procfs.h"
#include "fs/devfs.h"
#include "fs/storage_automount.h"
#include "net/lwip_port.h"
#include "drivers/virtio_net.h"
#include <posix/unistd.h>
#include "sched/coroutine.h"
#include "../llm/llm_runtime.h"
#include "../agent/agent_core.h"
#include "../evolution/evolution_plane.h"
#include "../container/lc_layer.h"
#include "../ui/shell.h"
#include "../ui/font.h"
#include "../lib/string.h"

static ui_shell_t       g_shell;
static llm_ctx_t        g_llm_ctx;
static agent_system_t   g_agent_sys;

static void print_banner(void)
{
    klog("\n");
    klog("  ==========================================\n");
    klog("  AevOS v%s — Autonomous Evolving OS\n", AEVOS_VERSION_STRING);
    klog("  ==========================================\n");
    klog("\n");
}

static void print_memory_info(boot_info_t *bi)
{
    uint64_t usable = 0;
    for (uint32_t i = 0; i < bi->mmap.count; i++) {
        if (bi->mmap.entries[i].type == MMAP_USABLE)
            usable += bi->mmap.entries[i].length;
    }

    klog("[mem] total %llu MB, usable %llu MB\n",
         bi->total_memory / (1024 * 1024),
         usable / (1024 * 1024));
    klog("[mem] kernel image at %p, size %llu KB\n",
         (void *)bi->kernel_phys_base,
         bi->kernel_size / 1024);
}

static void shell_coroutine(void *arg)
{
    ui_shell_t *shell = (ui_shell_t *)arg;
    shell_main_loop(shell);
}

void NORETURN kernel_main(boot_info_t *bi)
{
#if defined(__mips64)
    /*
     * Direct-boot path: boot_shim returns a kseg0 pointer (already valid).
     * UEFI path (if ever used): convert low physical address to kseg0.
     */
    if ((uintptr_t)bi < PHYS_MAP_BASE)
        bi = (boot_info_t *)((uintptr_t)bi + PHYS_MAP_BASE);
#endif

    if (bi->magic != BOOT_MAGIC) { /* 0xAE05B007 */
        for (;;) arch_panic_stop();
    }

    /* Phase 1: Core hardware */

    serial_init(SERIAL_PORT);
    print_banner();
    klog("[boot] boot_info magic ok (0x%llx)\n", bi->magic);
    print_memory_info(bi);

    klog("[cpu] GDT init\n");
    gdt_init();

    klog("[cpu] IDT + PIC init\n");
    idt_init();
    pic_init();

    /* Phase 2: Memory management */

    klog("[mm] PMM init (%u mmap entries)\n", bi->mmap.count);
    pmm_init(bi);

    klog("[mm] VMM init (identity map + high half + framebuffer)\n");
    vmm_init(bi);

#if defined(__aarch64__)
    /*
     * TTBR0 (bootloader identity map) pages live in EfiLoaderData which the
     * PMM now considers free.  Switch the boot_info pointer to the
     * PHYS_MAP_BASE mapping (TTBR1) so we no longer depend on TTBR0.
     */
    bi = (boot_info_t *)((uintptr_t)bi + PHYS_MAP_BASE);
#endif
    /* mips64: bi was already converted to xkphys at kernel_main entry */

    klog("[mm] slab allocator init\n");
    slab_init();

    /* Phase 3: Drivers */

    klog("[tmr] timer %u Hz\n", TIMER_FREQ_HZ);
    timer_init(TIMER_FREQ_HZ);

    klog("[locale] keyboard layout init\n");
    locale_init();

    klog("[hid] keyboard + mouse\n");
    hid_init();

    klog("[pci] PCI scan\n");
    pci_init();
    uint32_t pci_count = pci_get_device_count();
    klog("[pci] %u devices\n", pci_count);

    klog("[net] network stack init\n");
    net_init();
    if (virtio_net_init() != 0) {
        klog("[net] virtio-net not available, PCI scan only\n");
        net_detect_pci();
    }

    klog("[nvme] probing controllers\n");
    if (nvme_init())
        klog("[nvme] NVMe controller initialized\n");
    else
        klog("[nvme] No NVMe controllers found\n");

#if defined(__x86_64__)
    klog("[ahci] probing SATA (AHCI)\n");
    if (ahci_init())
        klog("[ahci] SATA port ready\n");
    else
        klog("[ahci] No AHCI disk or init failed\n");
#endif

    klog("[snd] audio probe\n");
    if (hda_init())
        klog("[snd]  HD Audio device initialized\n");
    else
        klog("[snd]  No HD Audio device (PC speaker available)\n");

    framebuffer_t fb_handoff;
    memcpy(&fb_handoff, &bi->fb, sizeof(framebuffer_t));

    /* Save the raw physical base before remapping for virtio-gpu backing */
    uint64_t fb_phys_base = (uint64_t)(uintptr_t)fb_handoff.base;

#if defined(__aarch64__) || defined(__mips64)
    /*
     * On aarch64/mips64, the GOP FrameBufferBase is a raw physical
     * address.  Remap it through PHYS_MAP_BASE so access goes via
     * the kernel's direct physical mapping segment.
     */
    if (fb_handoff.base)
        fb_handoff.base = (uint32_t *)(uintptr_t)(PHYS_MAP_BASE + fb_phys_base);
#endif

    klog("[fb] Initializing framebuffer: %ux%u @ %u bpp base=%p (phys=%llx)\n",
         fb_handoff.width, fb_handoff.height, fb_handoff.bpp,
         (void *)fb_handoff.base, (unsigned long long)fb_phys_base);

    /*
     * If GOP failed (dimensions are zero), try to bring up virtio-gpu
     * with default dimensions anyway — common on aarch64 virt where
     * some firmware builds lack a virtio-gpu GOP driver.
     */
    uint32_t gpu_width  = fb_handoff.width  ? fb_handoff.width  : 1024;
    uint32_t gpu_height = fb_handoff.height ? fb_handoff.height : 768;

    fb_init(&fb_handoff);

    /*
     * Initialize virtio-GPU driver.
     *
     * On virtio-gpu platforms the GOP FrameBufferBase points to device
     * BAR memory which cannot be used as a DMA backing store.  Instead
     * we use the kernel-allocated back buffer (guest RAM) as the
     * virtio-gpu resource backing.  Pipeline:
     *   draw → back_buffer → virtio transfer+flush → display.
     */
    klog("[gpu] probing virtio-gpu\n");
    fb_ctx_t *fb_ctx = fb_get_ctx();
    uint64_t gpu_backing_phys = 0;

    if (fb_ctx->back_buffer) {
        gpu_backing_phys = (uint64_t)(uintptr_t)fb_ctx->back_buffer;
        if (gpu_backing_phys >= PHYS_MAP_BASE)
            gpu_backing_phys -= PHYS_MAP_BASE;
        klog("[gpu] using back_buffer as backing: phys=%llx\n",
             (unsigned long long)gpu_backing_phys);
    } else {
        /*
         * No back buffer (GOP failed or fb_init returned early).
         * Allocate a fresh page-aligned buffer for virtio-gpu backing.
         */
        uint64_t fb_bytes = (uint64_t)gpu_width * gpu_height * 4;
        uint64_t fb_pages = (fb_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        gpu_backing_phys = pmm_alloc_pages(fb_pages);
        if (gpu_backing_phys) {
            uint32_t *virt = (uint32_t *)(gpu_backing_phys + PHYS_MAP_BASE);
            memset(virt, 0, (size_t)(fb_pages * PAGE_SIZE));

            fb_handoff.base   = virt;
            fb_handoff.width  = gpu_width;
            fb_handoff.height = gpu_height;
            fb_handoff.pitch  = gpu_width * 4;
            fb_handoff.bpp    = 32;
            fb_init(&fb_handoff);
            klog("[gpu] allocated %llu KB backing buffer at phys=%llx\n",
                 (unsigned long long)(fb_bytes / 1024),
                 (unsigned long long)gpu_backing_phys);
        } else {
            klog("[gpu] WARNING: cannot allocate GPU backing buffer\n");
        }
    }

    if (virtio_gpu_init(gpu_backing_phys,
                        gpu_width, gpu_height) == 0) {
        klog("[gpu] virtio-gpu display active\n");
        fb_swap_buffers();
    } else {
        klog("[gpu] virtio-gpu not available (using direct framebuffer)\n");
    }

    hid_set_mouse_bounds((int32_t)fb_handoff.width, (int32_t)fb_handoff.height);

    /* Phase 4: Filesystems and scheduling */

    klog("[vfs] VFS init\n");
    vfs_init();
    procfs_init();
    devfs_init();

    block_storage_register_default();
    storage_automount_all();

    klog("[posix] POSIX.1-style I/O layer (errno, open/read/write/stat, …)\n");
    posix_init();

    klog("[sched] coroutine scheduler init\n");
    coro_init();

    /* Phase 5: AI subsystem */

    klog("[llm] runtime init\n");
    klog("[llm] model %s\n", bi->cfg.model_path);
    klog("[llm] context %u tokens\n", bi->cfg.n_ctx);

    memset(&g_llm_ctx, 0, sizeof(g_llm_ctx));
    int llm_ok = llm_init(&g_llm_ctx, bi->cfg.model_path);
    if (llm_ok == 0)
        klog("[llm] ready\n");
    else
        klog("[llm] init failed (%d), continuing without LLM\n",
             llm_ok);

    /* Phase 6: Agent system */

    klog("[agent] subsystem init\n");
    agent_system_init(&g_agent_sys);
    evolution_plane_init();
    lc_layer_init();
    agent_t *default_agent = agent_create(&g_agent_sys, "aevos-default");
    if (default_agent) {
        default_agent->llm = &g_llm_ctx;
        klog("[agent] Agent '%s' created (id=%llu)\n",
             default_agent->name, default_agent->id);
    } else {
        klog("[agent] WARNING: Failed to create default agent\n");
    }

    /* Phase 7: UI Shell */

    klog("[ui] font subsystem\n");
    font_init();

    klog("[ui] shell init\n");
    fb_ctx_t *fb = fb_get_ctx();
    shell_init(&g_shell, fb, default_agent);
    klog("[ui] shell ready %ux%u\n", fb->width, fb->height);

    /* Phase 8: Start main loop */

    klog("[sched] spawn ui-shell coroutine\n");
    if (!coro_create("ui-shell", shell_coroutine, &g_shell, 128))
        kpanic("ui-shell: coro_create failed (out of memory?) — cannot start desktop\n");

    klog("[boot] handoff to scheduler\n");
    klog("===========================================\n\n");

    arch_enable_irq();
    scheduler_run();

    kpanic("Scheduler returned unexpectedly");
    for (;;) arch_panic_stop();
}
