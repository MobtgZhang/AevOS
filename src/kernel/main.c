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
#include "drivers/audio.h"
#include "drivers/virtio_gpu.h"
#include "fs/vfs.h"
#include "sched/coroutine.h"
#include "../llm/llm_runtime.h"
#include "../agent/agent_core.h"
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

    klog("[mm] slab allocator init\n");
    slab_init();

    /* Phase 3: Drivers */

    klog("[tmr] timer %u Hz\n", TIMER_FREQ_HZ);
    timer_init(TIMER_FREQ_HZ);

    klog("[hid] keyboard + mouse\n");
    hid_init();

    klog("[pci] PCI scan\n");
    pci_init();
    uint32_t pci_count = pci_get_device_count();
    klog("[pci] %u devices\n", pci_count);

    klog("[nvme] probing controllers\n");
    if (nvme_init())
        klog("[nvme] NVMe controller initialized\n");
    else
        klog("[nvme] No NVMe controllers found\n");

    klog("[snd] audio probe\n");
    if (hda_init())
        klog("[snd]  HD Audio device initialized\n");
    else
        klog("[snd]  No HD Audio device (PC speaker available)\n");

    framebuffer_t fb_handoff;
    memcpy(&fb_handoff, &bi->fb, sizeof(framebuffer_t));
    klog("[fb] Initializing framebuffer: %ux%u @ %u bpp\n",
         fb_handoff.width, fb_handoff.height, fb_handoff.bpp);
    fb_init(&fb_handoff);

    /* Initialize virtio-GPU driver for platforms without legacy VGA (LoongArch, etc.) */
    klog("[gpu] probing virtio-gpu\n");
    uint64_t fb_phys = (uint64_t)(uintptr_t)fb_handoff.base;
#if !defined(__x86_64__)
    /* On non-x86, the GOP framebuffer addr may need PHYS_MAP_BASE stripped */
    if (fb_phys >= PHYS_MAP_BASE)
        fb_phys -= PHYS_MAP_BASE;
#endif
    if (virtio_gpu_init(fb_phys, fb_handoff.width, fb_handoff.height) == 0)
        klog("[gpu] virtio-gpu display active\n");
    else
        klog("[gpu] virtio-gpu not available (using direct framebuffer)\n");

    hid_set_mouse_bounds((int32_t)fb_handoff.width, (int32_t)fb_handoff.height);

    /* Phase 4: Filesystems and scheduling */

    klog("[vfs] VFS init\n");
    vfs_init();

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
    coro_create("ui-shell", shell_coroutine, &g_shell, 128);

    klog("[boot] handoff to scheduler\n");
    klog("===========================================\n\n");

    arch_enable_irq();
    scheduler_run();

    kpanic("Scheduler returned unexpectedly");
    for (;;) arch_panic_stop();
}
