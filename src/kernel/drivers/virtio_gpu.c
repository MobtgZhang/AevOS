#include "virtio_gpu.h"
#include "pci.h"
#include "../arch/arch.h"
#include "../arch/io.h"
#include "../klog.h"
#include "../mm/pmm.h"
#include "../mm/slab.h"
#include "../../lib/string.h"
#include <aevos/config.h>

/*
 * Minimal virtio-GPU 2D driver — synchronous, polling-based.
 *
 * After UEFI ExitBootServices, the firmware's virtio-gpu driver is gone.
 * We re-initialize the device from scratch: reset, negotiate features,
 * create a 2D resource backed by the GOP framebuffer, configure scanout,
 * and provide a flush function for display updates.
 */

/* ── Virtio PCI ──────────────────────────────────────── */

#define VIRTIO_PCI_VENDOR         0x1AF4
#define VIRTIO_GPU_PCI_DID        0x1050  /* transitional */
#define VIRTIO_GPU_PCI_DID_MOD    0x1040  /* modern base; +0x10 = GPU */

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

/* Device status bits */
#define VIRTIO_STATUS_ACK         1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_DRIVER_OK   4

/* feature bit 32 → selector 1, bit 0 (VirtIO 1.0); required by modern QEMU virtio-pci */
#define VIRTIO_F_VERSION_1_BIT32  1u

/* ── Virtqueue ───────────────────────────────────────── */

#define VQ_SIZE 64

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_SIZE];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VQ_SIZE];
} __attribute__((packed));

/* ── Virtio-GPU commands ─────────────────────────────── */

#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO      0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    0x0101
#define VIRTIO_GPU_CMD_SET_SCANOUT           0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH        0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106

#define VIRTIO_GPU_RESP_OK_NODATA            0x1100
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM     2

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x, y, width, height;
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* ── Driver state ────────────────────────────────────── */

static struct {
    bool ready;

    volatile uint8_t *common_cfg;
    volatile uint8_t *notify_base;
    uint32_t          notify_off_mult;
    volatile uint8_t *isr_cfg;
    volatile uint8_t *device_cfg;

    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint64_t            desc_phys;
    uint64_t            avail_phys;
    uint64_t            used_phys;

    uint16_t            last_used_idx;
    uint16_t            next_desc;
    uint16_t            queue_notify_off;

    uint32_t            resource_id;
    uint32_t            fb_width;
    uint32_t            fb_height;
    uint64_t            fb_phys;

    uint64_t            cmd_phys;
    uint8_t            *cmd_buf;
} gpu;

/* ── MMIO helpers ────────────────────────────────────── */

static inline uint8_t  vr8(volatile uint8_t *base, uint32_t off)  { return *(volatile uint8_t *)(base + off); }
static inline uint16_t vr16(volatile uint8_t *base, uint32_t off) { return *(volatile uint16_t *)(base + off); }
static inline uint32_t vr32(volatile uint8_t *base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline void vw8(volatile uint8_t *base, uint32_t off, uint8_t val)  { *(volatile uint8_t *)(base + off) = val; }
static inline void vw16(volatile uint8_t *base, uint32_t off, uint16_t val) { *(volatile uint16_t *)(base + off) = val; }
static inline void vw32(volatile uint8_t *base, uint32_t off, uint32_t val) { *(volatile uint32_t *)(base + off) = val; }
static inline void vw64(volatile uint8_t *base, uint32_t off, uint64_t val) { *(volatile uint64_t *)(base + off) = val; }

/* Common config register offsets */
#define CC_DEVICE_FEATURE_SEL  0x00
#define CC_DEVICE_FEATURE      0x04
#define CC_DRIVER_FEATURE_SEL  0x08
#define CC_DRIVER_FEATURE      0x0C
#define CC_MSIX_CONFIG         0x10
#define CC_NUM_QUEUES          0x12
#define CC_DEVICE_STATUS       0x14
#define CC_CONFIG_GEN          0x15
#define CC_QUEUE_SELECT        0x16
#define CC_QUEUE_SIZE          0x18
#define CC_QUEUE_MSIX_VECTOR   0x1A
#define CC_QUEUE_ENABLE        0x1C
#define CC_QUEUE_NOTIFY_OFF    0x1E
#define CC_QUEUE_DESC          0x20
#define CC_QUEUE_DRIVER        0x28
#define CC_QUEUE_DEVICE        0x30

/* ── Virtqueue operations ────────────────────────────── */

static void vq_submit_chain(uint16_t head)
{
    uint16_t ai = gpu.avail->idx % VQ_SIZE;
    gpu.avail->ring[ai] = head;
    __sync_synchronize();
    gpu.avail->idx++;
    __sync_synchronize();

    {
        uint16_t qsz = vr16(gpu.common_cfg, CC_QUEUE_SIZE);
        if (qsz == 0 || qsz > VQ_SIZE)
            qsz = VQ_SIZE;
        size_t dsz  = sizeof(struct virtq_desc) * qsz;
        size_t asz  = sizeof(uint16_t) * (3 + (size_t)qsz);
        arch_dcache_flush_range(gpu.desc, dsz);
        arch_dcache_flush_range(gpu.avail, asz);
    }

    /* Notify the device */
    vw16(gpu.notify_base, gpu.queue_notify_off * gpu.notify_off_mult, 0);
}

static void vq_wait_used(void)
{
    uint16_t qsz = vr16(gpu.common_cfg, CC_QUEUE_SIZE);
    if (qsz == 0 || qsz > VQ_SIZE)
        qsz = VQ_SIZE;
    size_t usz = sizeof(uint16_t) * 3 +
                 sizeof(struct virtq_used_elem) * (size_t)qsz;

    for (int timeout = 0; timeout < 10000000; timeout++) {
        arch_dcache_invalidate_range(gpu.used, usz);
        __sync_synchronize();
        if ((uint16_t)(gpu.used->idx - gpu.last_used_idx) != 0) {
            gpu.last_used_idx = gpu.used->idx;
            return;
        }
    }
    klog("[virtio-gpu] timeout waiting for used\n");
}

static void send_cmd(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len)
{
    memcpy(gpu.cmd_buf, cmd, cmd_len);
    memset(gpu.cmd_buf + cmd_len, 0, resp_len);
    arch_dcache_flush_range(gpu.cmd_buf, (size_t)cmd_len + resp_len);

    uint16_t d0 = gpu.next_desc;
    uint16_t d1 = (d0 + 1) % VQ_SIZE;
    gpu.next_desc = (d1 + 1) % VQ_SIZE;

    gpu.desc[d0].addr  = gpu.cmd_phys;
    gpu.desc[d0].len   = cmd_len;
    gpu.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    gpu.desc[d0].next  = d1;

    gpu.desc[d1].addr  = gpu.cmd_phys + cmd_len;
    gpu.desc[d1].len   = resp_len;
    gpu.desc[d1].flags = VIRTQ_DESC_F_WRITE;
    gpu.desc[d1].next  = 0;

    vq_submit_chain(d0);
    vq_wait_used();

    if (resp && resp_len > 0) {
        arch_dcache_invalidate_range(gpu.cmd_buf + cmd_len, resp_len);
        memcpy(resp, gpu.cmd_buf + cmd_len, resp_len);
    }
}

/* ── Capability parsing ──────────────────────────────── */

static bool parse_capabilities(pci_device_t *dev)
{
    uint8_t cap_off = pci_read_config8(dev->bus, dev->device, dev->function, 0x34);
    cap_off &= ~3u;

    bool found_common = false;

    unsigned cap_hops = 0;
    while (cap_off && cap_off < 0xFF && cap_hops++ < 64) {
        uint8_t cap_id = pci_read_config8(dev->bus, dev->device, dev->function, cap_off);

        if (cap_id == 0x09) { /* VIRTIO_PCI_CAP_ID */
            uint8_t cfg_type = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 3);
            uint8_t bar      = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 4);
            uint32_t offset  = pci_read_config(dev->bus, dev->device, dev->function, (uint8_t)(cap_off + 8));
            uint32_t length  = pci_read_config(dev->bus, dev->device, dev->function, (uint8_t)(cap_off + 12));

            if (bar >= 6) {
                cap_off = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 1);
                continue;
            }
            uint32_t bar_raw = pci_read_config(dev->bus, dev->device, dev->function,
                                                 (uint8_t)(PCI_CFG_BAR0 + bar * 4u));
            if (bar_raw & PCI_BAR_IO) {
                cap_off = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 1);
                continue;
            }

            uint64_t bar_addr = dev->bar[bar];
            if (!bar_addr) {
                cap_off = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 1);
                continue;
            }

            void *mmio_v = pci_bar_to_mmio_vaddr(bar_addr);
            if (!mmio_v) {
                cap_off = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 1);
                continue;
            }
            volatile uint8_t *mmio = (volatile uint8_t *)mmio_v;
            volatile uint8_t *base = mmio + offset;

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                gpu.common_cfg = base;
                found_common = true;
                klog("[virtio-gpu] common cfg BAR%u off=0x%x len=0x%x\n", bar, offset, length);
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                gpu.notify_base = mmio + offset;
                gpu.notify_off_mult = pci_read_config(dev->bus, dev->device, dev->function, (uint8_t)(cap_off + 16));
                klog("[virtio-gpu] notify BAR%u off=0x%x mult=%u\n", bar, offset, gpu.notify_off_mult);
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                gpu.isr_cfg = base;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                gpu.device_cfg = base;
                break;
            }
        }

        uint8_t next = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 1);
        if (next == cap_off)
            break;
        cap_off = next;
    }

    return found_common && gpu.notify_base;
}

/* ── Initialization ──────────────────────────────────── */

int virtio_gpu_init(uint64_t fb_phys, uint32_t width, uint32_t height)
{
    memset(&gpu, 0, sizeof(gpu));

    /* Find virtio-gpu PCI device */
    pci_device_t *dev = pci_find_device(VIRTIO_PCI_VENDOR, VIRTIO_GPU_PCI_DID);
    if (!dev) {
        /* Try modern device ID */
        for (uint32_t i = 0; i < pci_get_device_count(); i++) {
            pci_device_t *d = pci_get_device(i);
            if (d && d->vendor_id == VIRTIO_PCI_VENDOR &&
                d->class_code == 0x03) {
                dev = d;
                break;
            }
        }
    }
    if (!dev) {
        klog("[virtio-gpu] no device found\n");
        return -1;
    }

    klog("[virtio-gpu] found device %x:%x at %u:%u.%u\n",
         dev->vendor_id, dev->device_id, dev->bus, dev->device, dev->function);

    /* Enable bus mastering and memory space */
    uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, PCI_CFG_COMMAND);
    cmd |= 0x06; /* Memory Space + Bus Master */
    pci_write_config(dev->bus, dev->device, dev->function, PCI_CFG_COMMAND, cmd);

    /* Parse capabilities */
    if (!parse_capabilities(dev)) {
        klog("[virtio-gpu] failed to parse virtio capabilities\n");
        return -1;
    }

    /* Reset device */
    vw8(gpu.common_cfg, CC_DEVICE_STATUS, 0);
    for (volatile int i = 0; i < 10000; i++) {}

    /* Acknowledge and set driver */
    vw8(gpu.common_cfg, CC_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    vw8(gpu.common_cfg, CC_DEVICE_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* VirtIO 1.0: read device features, offer a valid subset (same strategy as virtio-net). */
    __sync_synchronize();
    vw32(gpu.common_cfg, CC_DEVICE_FEATURE_SEL, 0);
    __sync_synchronize();
    uint32_t dev_f0 = vr32(gpu.common_cfg, CC_DEVICE_FEATURE);
    vw32(gpu.common_cfg, CC_DEVICE_FEATURE_SEL, 1);
    __sync_synchronize();
    uint32_t dev_f1 = vr32(gpu.common_cfg, CC_DEVICE_FEATURE);

    uint32_t drv_f0 = dev_f0;
    uint32_t drv_f1 = dev_f1;
    if (drv_f1 & VIRTIO_F_VERSION_1_BIT32)
        drv_f1 = VIRTIO_F_VERSION_1_BIT32;
    else
        drv_f1 = 0;

    vw32(gpu.common_cfg, CC_DRIVER_FEATURE_SEL, 0);
    vw32(gpu.common_cfg, CC_DRIVER_FEATURE, drv_f0);
    vw32(gpu.common_cfg, CC_DRIVER_FEATURE_SEL, 1);
    vw32(gpu.common_cfg, CC_DRIVER_FEATURE, drv_f1);

    vw8(gpu.common_cfg, CC_DEVICE_STATUS,
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    uint8_t status = vr8(gpu.common_cfg, CC_DEVICE_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        klog("[virtio-gpu] feature negotiation failed (dev_f0=%x dev_f1=%x)\n",
             dev_f0, dev_f1);
        return -1;
    }

    /* Set up virtqueue 0 (controlq); QueueSize is device-owned in v1 — do not write it. */
    vw16(gpu.common_cfg, CC_QUEUE_SELECT, 0);
    uint16_t qsz = vr16(gpu.common_cfg, CC_QUEUE_SIZE);
    if (qsz == 0 || qsz > VQ_SIZE) {
        klog("[virtio-gpu] bad control queue size %u (max %u)\n",
             (unsigned)qsz, (unsigned)VQ_SIZE);
        return -1;
    }

    /* Allocate virtqueue memory (physically contiguous) */
    size_t desc_bytes  = sizeof(struct virtq_desc) * qsz;
    size_t avail_bytes = sizeof(uint16_t) * (3 + qsz);
    size_t used_bytes  = sizeof(uint16_t) * 3 + sizeof(struct virtq_used_elem) * qsz;

    size_t total = ALIGN_UP(desc_bytes, 4096) + ALIGN_UP(avail_bytes, 4096) + ALIGN_UP(used_bytes, 4096);
    size_t pages = (total + 4095) / 4096;

    uint64_t vq_phys = pmm_alloc_pages(pages);
    if (!vq_phys) {
        klog("[virtio-gpu] cannot allocate virtqueue pages\n");
        return -1;
    }

    uint8_t *vq_virt = (uint8_t *)(vq_phys + PHYS_MAP_BASE);
    memset(vq_virt, 0, pages * 4096);

    gpu.desc  = (struct virtq_desc *)vq_virt;
    gpu.avail = (struct virtq_avail *)(vq_virt + ALIGN_UP(desc_bytes, 4096));
    gpu.used  = (struct virtq_used *)(vq_virt + ALIGN_UP(desc_bytes, 4096) + ALIGN_UP(avail_bytes, 4096));

    gpu.desc_phys  = vq_phys;
    gpu.avail_phys = vq_phys + ALIGN_UP(desc_bytes, 4096);
    gpu.used_phys  = vq_phys + ALIGN_UP(desc_bytes, 4096) + ALIGN_UP(avail_bytes, 4096);

    vw64(gpu.common_cfg, CC_QUEUE_DESC,   gpu.desc_phys);
    vw64(gpu.common_cfg, CC_QUEUE_DRIVER, gpu.avail_phys);
    vw64(gpu.common_cfg, CC_QUEUE_DEVICE, gpu.used_phys);
    vw16(gpu.common_cfg, CC_QUEUE_ENABLE, 1);

    gpu.queue_notify_off = vr16(gpu.common_cfg, CC_QUEUE_NOTIFY_OFF);
    gpu.last_used_idx = 0;
    gpu.next_desc = 0;

    /* Allocate command buffer (one page for commands + responses) */
    gpu.cmd_phys = pmm_alloc_pages(1);
    if (!gpu.cmd_phys) {
        klog("[virtio-gpu] cannot allocate command buffer\n");
        return -1;
    }
    gpu.cmd_buf = (uint8_t *)(gpu.cmd_phys + PHYS_MAP_BASE);
    memset(gpu.cmd_buf, 0, 4096);

    /* Mark device as ready */
    vw8(gpu.common_cfg, CC_DEVICE_STATUS,
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    gpu.fb_phys   = fb_phys;
    gpu.fb_width  = width;
    gpu.fb_height = height;
    gpu.resource_id = 1;

    klog("[virtio-gpu] device ready, creating framebuffer resource\n");

    /* Create 2D resource */
    struct virtio_gpu_resource_create_2d create = {
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D },
        .resource_id = gpu.resource_id,
        .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
        .width = width,
        .height = height,
    };
    struct virtio_gpu_ctrl_hdr resp;
    send_cmd(&create, sizeof(create), &resp, sizeof(resp));
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        klog("[virtio-gpu] resource_create_2d failed: 0x%x\n", resp.type);
        return -1;
    }

    /* Attach backing store */
    struct {
        struct virtio_gpu_resource_attach_backing cmd;
        struct virtio_gpu_mem_entry entry;
    } __attribute__((packed)) attach = {
        .cmd = {
            .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING },
            .resource_id = gpu.resource_id,
            .nr_entries = 1,
        },
        .entry = {
            .addr = fb_phys,
            .length = width * height * 4,
        },
    };
    send_cmd(&attach, sizeof(attach), &resp, sizeof(resp));
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        klog("[virtio-gpu] attach_backing failed: 0x%x\n", resp.type);
        return -1;
    }

    /* Set scanout */
    struct virtio_gpu_set_scanout scanout = {
        .hdr = { .type = VIRTIO_GPU_CMD_SET_SCANOUT },
        .r = { .x = 0, .y = 0, .width = width, .height = height },
        .scanout_id = 0,
        .resource_id = gpu.resource_id,
    };
    send_cmd(&scanout, sizeof(scanout), &resp, sizeof(resp));
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        klog("[virtio-gpu] set_scanout failed: 0x%x\n", resp.type);
        return -1;
    }

    gpu.ready = true;
    klog("[virtio-gpu] initialized %ux%u display\n", width, height);

    /* Initial flush */
    virtio_gpu_flush(0, 0, width, height);
    return 0;
}

void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!gpu.ready) return;

    /* Transfer to host */
    struct virtio_gpu_transfer_to_host_2d xfer = {
        .hdr = { .type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D },
        .r = { .x = x, .y = y, .width = w, .height = h },
        .offset = 0,
        .resource_id = gpu.resource_id,
    };
    struct virtio_gpu_ctrl_hdr resp;
    send_cmd(&xfer, sizeof(xfer), &resp, sizeof(resp));

    /* Flush */
    struct virtio_gpu_resource_flush flush = {
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_FLUSH },
        .r = { .x = x, .y = y, .width = w, .height = h },
        .resource_id = gpu.resource_id,
    };
    send_cmd(&flush, sizeof(flush), &resp, sizeof(resp));
}

bool virtio_gpu_available(void)
{
    return gpu.ready;
}
