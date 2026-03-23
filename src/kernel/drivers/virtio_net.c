/*
 * Minimal virtio-net (PCI modern) — polling RX/TX for the in-kernel stack.
 */

#include "virtio_net.h"
#include "pci.h"
#include "../klog.h"
#include "../mm/pmm.h"
#include "../net/lwip_port.h"
#include "../../lib/string.h"
#include <aevos/config.h>
#include <aevos/types.h>

#define VIRTIO_PCI_VENDOR      0x1AF4
#define VIRTIO_NET_PCI_DID_MOD 0x1041

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_STATUS_ACK         1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_DRIVER_OK   4

/* QEMU virtio-net 常见 queue size 为 256；不得向设备写入小于其最大值的 QueueSize（易致异常）。 */
#define VQ_MAX 256

#define VIRTIO_F_VERSION_1_BIT32  1u /* feature bit 32 → selector 1, bit 0 */

#define VIRTIO_NET_HDR_SIZE 12

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_MAX];
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VQ_MAX];
} __attribute__((packed));

struct vq {
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint64_t            desc_phys;
    uint64_t            avail_phys;
    uint64_t            used_phys;
    uint16_t            size;
    uint16_t            notify_off;
    uint16_t            last_used_idx; /* free-running, slot = idx % size */
};

static struct {
    bool                 ready;
    volatile uint8_t    *common_cfg;
    volatile uint8_t    *notify_base;
    uint32_t             notify_off_mult;
    struct vq            rxq;
    struct vq            txq;
    uint64_t             rx_buf_phys;
    uint8_t             *rx_buf_virt;
    uint32_t             rx_slot_size;
    uint64_t             tx_buf_phys;
    uint8_t             *tx_buf_virt;
    net_interface_t      iface;
} vn;

static inline uint8_t  vr8(volatile uint8_t *b, uint32_t o)  { return *(volatile uint8_t *)(b + o); }
static inline uint16_t vr16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t vr32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void vw8(volatile uint8_t *b, uint32_t o, uint8_t v)  { *(volatile uint8_t *)(b + o) = v; }
static inline void vw16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void vw32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void vw64(volatile uint8_t *b, uint32_t o, uint64_t v) { *(volatile uint64_t *)(b + o) = v; }

#define CC_DEVICE_FEATURE_SEL  0x00
#define CC_DEVICE_FEATURE      0x04
#define CC_DRIVER_FEATURE_SEL  0x08
#define CC_DRIVER_FEATURE      0x0C
#define CC_DEVICE_STATUS       0x14
#define CC_QUEUE_SELECT        0x16
#define CC_QUEUE_SIZE          0x18
#define CC_QUEUE_ENABLE        0x1C
#define CC_QUEUE_NOTIFY_OFF    0x1E
#define CC_QUEUE_DESC          0x20
#define CC_QUEUE_DRIVER        0x28
#define CC_QUEUE_DEVICE        0x30

static pci_device_t *find_virtio_net(void)
{
    for (uint32_t i = 0; i < pci_get_device_count(); i++) {
        pci_device_t *d = pci_get_device(i);
        if (!d || d->vendor_id != VIRTIO_PCI_VENDOR)
            continue;
        if (d->device_id == VIRTIO_NET_PCI_DID_MOD)
            return d;
        if (d->device_id >= 0x1000 && d->device_id <= 0x103F) {
            uint32_t sub = pci_read_config(d->bus, d->device, d->function, 0x2C);
            if ((sub >> 16) == 1)
                return d;
        }
    }
    return NULL;
}

static bool parse_caps(pci_device_t *dev, volatile uint8_t **common_out,
                       volatile uint8_t **notify_out, uint32_t *mult_out,
                       volatile uint8_t **devcfg_out)
{
    uint8_t cap_off = pci_read_config8(dev->bus, dev->device, dev->function, 0x34);
    cap_off &= ~3u;

    *common_out = NULL;
    *notify_out = NULL;
    *mult_out   = 0;
    *devcfg_out = NULL;

    unsigned cap_hops = 0;
    while (cap_off && cap_off < 0xFF && cap_hops++ < 64) {
        uint8_t cap_id = pci_read_config8(dev->bus, dev->device, dev->function, cap_off);
        if (cap_id == 0x09) {
            uint8_t cfg_type = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 3);
            uint8_t bar      = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 4);
            uint32_t offset  = pci_read_config(dev->bus, dev->device, dev->function, (uint8_t)(cap_off + 8));
            uint32_t length  = pci_read_config(dev->bus, dev->device, dev->function, (uint8_t)(cap_off + 12));

            (void)length;
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
                *common_out = base;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                *notify_out = mmio + offset;
                *mult_out = pci_read_config(dev->bus, dev->device, dev->function, (uint8_t)(cap_off + 16));
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                *devcfg_out = base;
                break;
            default:
                break;
            }
        }
        uint8_t next = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 1);
        if (next == cap_off)
            break;
        cap_off = next;
    }

    return *common_out && *notify_out;
}

static int alloc_vq(pci_device_t *dev, uint16_t qidx, struct vq *vq)
{
    vw16(vn.common_cfg, CC_QUEUE_SELECT, qidx);
    uint16_t qsz = vr16(vn.common_cfg, CC_QUEUE_SIZE);
    if (qsz == 0)
        return -1;
    /*
     * QueueSize 为设备报告的最大队列深度；驱动分配的环必须与之一致。
     * 若超过本驱动编译上限则放弃初始化（勿向 MMIO 写入更小的 QueueSize）。
     */
    if (qsz > VQ_MAX) {
        klog("[virtio-net] queue %u size %u > VQ_MAX %u\n",
             (unsigned)qidx, (unsigned)qsz, (unsigned)VQ_MAX);
        return -1;
    }

    size_t dsz  = sizeof(struct virtq_desc) * qsz;
    size_t asz  = sizeof(uint16_t) * (3 + (size_t)qsz);
    size_t usz  = sizeof(uint16_t) * 3 + sizeof(struct virtq_used_elem) * (size_t)qsz;

    size_t total = ALIGN_UP(dsz, 4096) + ALIGN_UP(asz, 4096) + ALIGN_UP(usz, 4096);
    size_t pages = (total + 4095) / 4096;

    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys)
        return -ENOMEM;

    uint8_t *virt = (uint8_t *)(phys + PHYS_MAP_BASE);
    memset(virt, 0, pages * 4096);

    vq->desc       = (struct virtq_desc *)virt;
    vq->avail      = (struct virtq_avail *)(virt + ALIGN_UP(dsz, 4096));
    vq->used       = (struct virtq_used *)(virt + ALIGN_UP(dsz, 4096) + ALIGN_UP(asz, 4096));
    vq->desc_phys  = phys;
    vq->avail_phys = phys + ALIGN_UP(dsz, 4096);
    vq->used_phys  = phys + ALIGN_UP(dsz, 4096) + ALIGN_UP(asz, 4096);
    vq->size       = qsz;
    vq->notify_off = vr16(vn.common_cfg, CC_QUEUE_NOTIFY_OFF);
    vq->last_used_idx = 0;

    vw64(vn.common_cfg, CC_QUEUE_DESC,   vq->desc_phys);
    vw64(vn.common_cfg, CC_QUEUE_DRIVER, vq->avail_phys);
    vw64(vn.common_cfg, CC_QUEUE_DEVICE, vq->used_phys);
    vw16(vn.common_cfg, CC_QUEUE_ENABLE, 1);

    (void)dev;
    return 0;
}

static void vq_kick(struct vq *vq)
{
    vw16(vn.notify_base, (uint32_t)vq->notify_off * vn.notify_off_mult, 0);
    __sync_synchronize();
}

static ssize_t vn_send_raw(const void *data, size_t len)
{
    if (!vn.ready || len > 2048)
        return -EIO;

    memset(vn.tx_buf_virt, 0, VIRTIO_NET_HDR_SIZE);
    memcpy(vn.tx_buf_virt + VIRTIO_NET_HDR_SIZE, data, len);
    size_t total = VIRTIO_NET_HDR_SIZE + len;

    struct vq *q = &vn.txq;
    q->desc[0].addr  = vn.tx_buf_phys;
    q->desc[0].len   = (uint32_t)total;
    q->desc[0].flags = 0;
    q->desc[0].next  = 0;

    uint16_t ai = q->avail->idx % q->size;
    q->avail->ring[ai] = 0;
    __sync_synchronize();
    q->avail->idx++;
    __sync_synchronize();
    vq_kick(q);

    for (int t = 0; t < 5000000; t++) {
        __sync_synchronize();
        if ((uint16_t)(q->used->idx - q->last_used_idx) != 0) {
            q->last_used_idx = q->used->idx;
            return (ssize_t)len;
        }
    }
    return -EIO;
}

/*
 * Pull one completed RX buffer, copy Ethernet payload for stack, re-queue buffer.
 */
static ssize_t vn_recv_raw(void *buf, size_t max_len)
{
    struct vq *q = &vn.rxq;
    __sync_synchronize();
    /* uint16 下标单调递增，避免 idx 回绕时误判为“无完成项” */
    if ((uint16_t)(q->used->idx - q->last_used_idx) == 0)
        return 0;

    uint16_t idx = (uint16_t)(q->last_used_idx % q->size);
    uint32_t d   = q->used->ring[idx].id;
    uint32_t l   = q->used->ring[idx].len;
    q->last_used_idx++;

    if (d >= (uint32_t)q->size) {
        klog("[virtio-net] RX bad desc id %u (q=%u), disabling\n",
             (unsigned)d, (unsigned)q->size);
        vn.ready = false;
        return 0;
    }

    if (l <= VIRTIO_NET_HDR_SIZE)
        goto repost;

    size_t eth_len = l - VIRTIO_NET_HDR_SIZE;
    uint8_t *pkt = vn.rx_buf_virt + (uintptr_t)d * vn.rx_slot_size;
    size_t copy = eth_len < max_len ? eth_len : max_len;
    memcpy(buf, pkt + VIRTIO_NET_HDR_SIZE, copy);

repost:
    q->desc[d].addr  = vn.rx_buf_phys + (uint64_t)d * vn.rx_slot_size;
    q->desc[d].len   = vn.rx_slot_size;
    q->desc[d].flags = VIRTQ_DESC_F_WRITE;
    q->desc[d].next  = 0;

    uint16_t ai = q->avail->idx % q->size;
    q->avail->ring[ai] = (uint16_t)d;
    __sync_synchronize();
    q->avail->idx++;
    __sync_synchronize();
    vq_kick(q);

    return (ssize_t)(l > VIRTIO_NET_HDR_SIZE ? (l - VIRTIO_NET_HDR_SIZE) : 0);
}

void virtio_net_poll(void)
{
    if (!vn.ready)
        return;

    uint8_t frame[2048];
    /* 防止异常队列状态导致 UI 线程饿死 */
    for (unsigned iter = 0; iter < 64; iter++) {
        ssize_t n = vn_recv_raw(frame, sizeof(frame));
        if (n <= 0)
            break;
        net_process_frame(frame, (size_t)n);
    }
}

bool virtio_net_available(void)
{
    return vn.ready;
}

int virtio_net_init(void)
{
    memset(&vn, 0, sizeof(vn));

    pci_device_t *dev = find_virtio_net();
    if (!dev) {
        klog("[virtio-net] no PCI device\n");
        return -1;
    }

    volatile uint8_t *devcfg = NULL;
    if (!parse_caps(dev, &vn.common_cfg, &vn.notify_base, &vn.notify_off_mult, &devcfg)) {
        klog("[virtio-net] missing virtio caps\n");
        return -1;
    }

    uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, PCI_CFG_COMMAND);
    cmd |= 0x06;
    pci_write_config(dev->bus, dev->device, dev->function, PCI_CFG_COMMAND, cmd);

    vw8(vn.common_cfg, CC_DEVICE_STATUS, 0);
    for (volatile int i = 0; i < 10000; i++) {}

    vw8(vn.common_cfg, CC_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    vw8(vn.common_cfg, CC_DEVICE_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* 读设备特性（置 DRIVER 之后，VirtIO 1.0 要求） */
    __sync_synchronize();
    vw32(vn.common_cfg, CC_DEVICE_FEATURE_SEL, 0);
    __sync_synchronize();
    uint32_t dev_f0 = vr32(vn.common_cfg, CC_DEVICE_FEATURE);
    vw32(vn.common_cfg, CC_DEVICE_FEATURE_SEL, 1);
    __sync_synchronize();
    uint32_t dev_f1 = vr32(vn.common_cfg, CC_DEVICE_FEATURE);

    /*
     * QEMU 要求驱动写入「设备提供特性的子集」；全 0 往往无法置 FEATURES_OK。
     * 最小栈驱动：接受设备提供的低 32 位 +（若存在）VERSION_1。
     */
    uint32_t drv_f0 = dev_f0;
    uint32_t drv_f1 = dev_f1;
    if (drv_f1 & VIRTIO_F_VERSION_1_BIT32)
        drv_f1 = VIRTIO_F_VERSION_1_BIT32;
    else
        drv_f1 = 0;

    vw32(vn.common_cfg, CC_DRIVER_FEATURE_SEL, 0);
    vw32(vn.common_cfg, CC_DRIVER_FEATURE, drv_f0);
    vw32(vn.common_cfg, CC_DRIVER_FEATURE_SEL, 1);
    vw32(vn.common_cfg, CC_DRIVER_FEATURE, drv_f1);

    vw8(vn.common_cfg, CC_DEVICE_STATUS,
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    if (!(vr8(vn.common_cfg, CC_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        klog("[virtio-net] feature negotiation failed (dev_f0=%x dev_f1=%x)\n",
             dev_f0, dev_f1);
        return -1;
    }

    if (alloc_vq(dev, 0, &vn.rxq) < 0) {
        klog("[virtio-net] RX queue setup failed\n");
        return -1;
    }
    if (alloc_vq(dev, 1, &vn.txq) < 0) {
        klog("[virtio-net] TX queue setup failed\n");
        return -1;
    }

    vn.rx_slot_size = 2048;
    size_t rx_bytes = (size_t)vn.rxq.size * vn.rx_slot_size;
    size_t rx_pages = (rx_bytes + 4095) / 4096;
    vn.rx_buf_phys = pmm_alloc_pages(rx_pages);
    if (!vn.rx_buf_phys) {
        klog("[virtio-net] RX buffer alloc failed\n");
        return -1;
    }
    vn.rx_buf_virt = (uint8_t *)(vn.rx_buf_phys + PHYS_MAP_BASE);
    memset(vn.rx_buf_virt, 0, rx_pages * 4096);

    for (uint16_t i = 0; i < vn.rxq.size; i++) {
        vn.rxq.desc[i].addr  = vn.rx_buf_phys + (uint64_t)i * vn.rx_slot_size;
        vn.rxq.desc[i].len   = vn.rx_slot_size;
        vn.rxq.desc[i].flags = VIRTQ_DESC_F_WRITE;
        vn.rxq.desc[i].next  = 0;
        vn.rxq.avail->ring[i] = i;
    }
    __sync_synchronize();
    vn.rxq.avail->idx = vn.rxq.size;
    vn.rxq.last_used_idx = 0;
    vq_kick(&vn.rxq);

    vn.tx_buf_phys = pmm_alloc_pages(1);
    if (!vn.tx_buf_phys) {
        klog("[virtio-net] TX buffer alloc failed\n");
        return -1;
    }
    vn.tx_buf_virt = (uint8_t *)(vn.tx_buf_phys + PHYS_MAP_BASE);

    if (devcfg) {
        for (int i = 0; i < ETH_ADDR_LEN; i++)
            vn.iface.mac[i] = devcfg[i];
    }
    /* QEMU often provides MAC; if zero, use local admin range */
    if ((vn.iface.mac[0] | vn.iface.mac[1] | vn.iface.mac[2] |
         vn.iface.mac[3] | vn.iface.mac[4] | vn.iface.mac[5]) == 0) {
        vn.iface.mac[0] = 0x52;
        vn.iface.mac[1] = 0x54;
        vn.iface.mac[2] = 0x00;
        vn.iface.mac[3] = 0x12;
        vn.iface.mac[4] = 0x34;
        vn.iface.mac[5] = 0x56;
    }

    vn.iface.ip       = IP4(10, 0, 2, 15);
    vn.iface.netmask  = IP4(255, 255, 255, 0);
    vn.iface.gateway  = IP4(10, 0, 2, 2);
    vn.iface.is_up    = true;
    vn.iface.send_raw = vn_send_raw;
    vn.iface.recv_raw = vn_recv_raw;

    vw8(vn.common_cfg, CC_DEVICE_STATUS,
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    vn.ready = true;
    net_set_interface(&vn.iface);
    klog("[virtio-net] ready MAC %x:%x:%x:%x:%x:%x\n",
         (unsigned)vn.iface.mac[0], (unsigned)vn.iface.mac[1],
         (unsigned)vn.iface.mac[2], (unsigned)vn.iface.mac[3],
         (unsigned)vn.iface.mac[4], (unsigned)vn.iface.mac[5]);
    return 0;
}
