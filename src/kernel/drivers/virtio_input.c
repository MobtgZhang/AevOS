/*
 * VirtIO input (PCI): keyboard and relative mouse for QEMU virt-class machines.
 * LoongArch / AArch64 / RISC-V have no PS/2; USB HID is not implemented,
 * so virtio-keyboard-pci + virtio-mouse-pci supply desktop input.
 */

#include "virtio_input.h"
#include "virtio_pci_helpers.h"
#include "hid.h"
#include "pci.h"
#include "../klog.h"
#include "../mm/pmm.h"
#include "../../lib/string.h"
#include <aevos/config.h>
#include <aevos/types.h>

#define VIRTIO_PCI_VENDOR        0x1AF4
#define VIRTIO_INPUT_PCI_MOD     0x1042
#define VIRTIO_INPUT_PCI_LEGACY  0x1052
#define VIRTIO_ID_INPUT          18

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_STATUS_ACK          1
#define VIRTIO_STATUS_DRIVER       2
#define VIRTIO_STATUS_FEATURES_OK  8
#define VIRTIO_STATUS_DRIVER_OK    4

#define VIRTIO_F_VERSION_1_BIT32   1u

#define VQ_MAX           128
#define VI_SLOT_BYTES    256
#define VI_MAX_DEVICES   4

#define EV_SYN   0
#define EV_KEY   1
#define EV_REL   2
#define EV_ABS   3

#define SYN_REPORT 0

#define REL_X      0
#define REL_Y      1
#define REL_HWHEEL 6
#define REL_WHEEL  8

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

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

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

struct vi_vq {
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint64_t            desc_phys;
    uint64_t            avail_phys;
    uint64_t            used_phys;
    uint16_t            size;
    uint16_t            notify_off;
    uint16_t            last_used_idx;
};

struct vi_softc {
    bool                 ready;
    volatile uint8_t    *common_cfg;
    volatile uint8_t    *notify_base;
    uint32_t             notify_off_mult;
    struct vi_vq         evq;
    uint64_t             buf_phys;
    uint8_t             *buf_virt;
};

static struct vi_softc vi_dev[VI_MAX_DEVICES];
static unsigned      vi_count;
static bool          vi_global_init_done;

static inline uint8_t  vr8(volatile uint8_t *b, uint32_t o)  { return *(volatile uint8_t *)(b + o); }
static inline uint16_t vr16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t vr32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void vw8(volatile uint8_t *b, uint32_t o, uint8_t v)   { *(volatile uint8_t *)(b + o) = v; }
static inline void vw16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void vw32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }

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

static bool vi_is_virtio_input(pci_device_t *d)
{
    if (d->vendor_id != VIRTIO_PCI_VENDOR)
        return false;
    if (d->device_id == VIRTIO_INPUT_PCI_MOD || d->device_id == VIRTIO_INPUT_PCI_LEGACY)
        return true;
    uint32_t sub = pci_read_config(d->bus, d->device, d->function, 0x2C);
    uint16_t sv = sub & 0xFFFF;
    uint16_t sd = (uint16_t)((sub >> 16) & 0xFFFF);
    return sv == VIRTIO_PCI_VENDOR && sd == VIRTIO_ID_INPUT;
}

static keycode_t linux_evdev_key_to_aevos(uint16_t c)
{
    switch (c) {
    case 96:  return KEY_ENTER;   /* KEY_KPENTER */
    case 97:  return KEY_RCTRL;
    case 100: return KEY_RALT;
    case 102: return KEY_HOME;
    case 103: return KEY_UP;
    case 104: return KEY_PGUP;
    case 105: return KEY_LEFT;
    case 106: return KEY_RIGHT;
    case 107: return KEY_END;
    case 108: return KEY_DOWN;
    case 109: return KEY_PGDN;
    case 110: return KEY_INSERT;
    case 111: return KEY_DELETE;
    default:
        break;
    }
    if (c >= 1 && c <= 88)
        return (keycode_t)c;
    return KEY_NONE;
}

static void vi_handle_evdev(uint16_t type, uint16_t code, int32_t value)
{
    switch (type) {
    case EV_KEY:
        if (code >= BTN_LEFT && code <= BTN_MIDDLE) {
            uint8_t bit = (code == BTN_LEFT)   ? MOUSE_BTN_LEFT
                        : (code == BTN_RIGHT)  ? MOUSE_BTN_RIGHT
                        : MOUSE_BTN_MIDDLE;
            uint8_t nb = input_get_mouse_buttons();
            if (value)
                nb = (uint8_t)(nb | bit);
            else
                nb = (uint8_t)(nb & ~bit);
            hid_feed_mouse_buttons(nb);
            return;
        }
        {
            keycode_t kc = linux_evdev_key_to_aevos(code);
            if (kc != KEY_NONE)
                hid_feed_key(kc, value != 0);
        }
        break;
    case EV_REL:
        if (code == REL_X)
            hid_feed_mouse_rel(value, 0);
        else if (code == REL_Y)
            hid_feed_mouse_rel(0, value);
        else if (code == REL_WHEEL || code == REL_HWHEEL)
            hid_feed_mouse_scroll(value);
        break;
    default:
        break;
    }
}

static bool vi_parse_caps(pci_device_t *dev, struct vi_softc *s)
{
    uint8_t cap_off = pci_read_config8(dev->bus, dev->device, dev->function, 0x34);
    cap_off &= ~3u;

    s->common_cfg  = NULL;
    s->notify_base = NULL;
    s->notify_off_mult = 0;

    unsigned hops = 0;
    while (cap_off && cap_off < 0xFF && hops++ < 64) {
        uint8_t cap_id = pci_read_config8(dev->bus, dev->device, dev->function, cap_off);
        if (cap_id == 0x09) {
            uint8_t cfg_type = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 3);
            uint8_t bar      = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 4);
            uint32_t offset  = pci_read_config(dev->bus, dev->device, dev->function, (uint8_t)(cap_off + 8));

            if (bar < 6) {
                uint32_t bar_raw = pci_read_config(dev->bus, dev->device, dev->function,
                                                   (uint8_t)(PCI_CFG_BAR0 + bar * 4u));
                if (!(bar_raw & PCI_BAR_IO)) {
                    uint64_t bar_addr = dev->bar[bar];
                    if (bar_addr) {
                        void *mmio_v = pci_bar_to_mmio_vaddr(bar_addr);
                        if (mmio_v) {
                            volatile uint8_t *mmio = (volatile uint8_t *)mmio_v;
                            volatile uint8_t *base = mmio + offset;
                            switch (cfg_type) {
                            case VIRTIO_PCI_CAP_COMMON_CFG:
                                s->common_cfg = base;
                                break;
                            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                                s->notify_base = mmio + offset;
                                s->notify_off_mult = pci_read_config(dev->bus, dev->device, dev->function,
                                                                     (uint8_t)(cap_off + 16));
                                break;
                            default:
                                break;
                            }
                        }
                    }
                }
            }
        }
        uint8_t next = pci_read_config8(dev->bus, dev->device, dev->function, cap_off + 1);
        if (next == cap_off)
            break;
        cap_off = next;
    }
    return s->common_cfg && s->notify_base;
}

static void vi_vq_kick(struct vi_softc *s)
{
    /* Full barrier before notify so weak ISAs see avail/descriptor writes first. */
    __sync_synchronize();
    vw16(s->notify_base, (uint32_t)s->evq.notify_off * s->notify_off_mult, 0);
    __sync_synchronize();
}

static int vi_alloc_evq(struct vi_softc *s, pci_device_t *dev)
{
    volatile uint8_t *cc = s->common_cfg;
    vw16(cc, CC_QUEUE_SELECT, 0);
    uint16_t qsz = vr16(cc, CC_QUEUE_SIZE);
    if (qsz == 0 || qsz > VQ_MAX)
        return -1;

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

    struct vi_vq *q = &s->evq;
    q->desc       = (struct virtq_desc *)virt;
    q->avail      = (struct virtq_avail *)(virt + ALIGN_UP(dsz, 4096));
    q->used       = (struct virtq_used *)(virt + ALIGN_UP(dsz, 4096) + ALIGN_UP(asz, 4096));
    q->desc_phys  = phys;
    q->avail_phys = phys + ALIGN_UP(dsz, 4096);
    q->used_phys  = phys + ALIGN_UP(dsz, 4096) + ALIGN_UP(asz, 4096);
    q->size       = qsz;
    q->notify_off = vr16(cc, CC_QUEUE_NOTIFY_OFF);
    q->last_used_idx = 0;

    virtio_pci_common_cfg_write64(cc, CC_QUEUE_DESC,   q->desc_phys);
    virtio_pci_common_cfg_write64(cc, CC_QUEUE_DRIVER, q->avail_phys);
    virtio_pci_common_cfg_write64(cc, CC_QUEUE_DEVICE, q->used_phys);
    vw16(cc, CC_QUEUE_ENABLE, 1);
    (void)dev;
    return 0;
}

static bool vi_setup_pci_device(pci_device_t *dev, struct vi_softc *s)
{
    memset(s, 0, sizeof(*s));

    if (!vi_parse_caps(dev, s))
        return false;

    uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, PCI_CFG_COMMAND);
    pci_write_config(dev->bus, dev->device, dev->function, PCI_CFG_COMMAND, cmd | 0x06);

    volatile uint8_t *cc = s->common_cfg;
    vw8(cc, CC_DEVICE_STATUS, 0);
    for (volatile int i = 0; i < 10000; i++) {}

    vw8(cc, CC_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    vw8(cc, CC_DEVICE_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    __sync_synchronize();
    vw32(cc, CC_DEVICE_FEATURE_SEL, 0);
    __sync_synchronize();
    uint32_t dev_f0 = vr32(cc, CC_DEVICE_FEATURE);
    vw32(cc, CC_DEVICE_FEATURE_SEL, 1);
    __sync_synchronize();
    uint32_t dev_f1 = vr32(cc, CC_DEVICE_FEATURE);

    uint32_t drv_f0 = dev_f0;
    uint32_t drv_f1 = dev_f1;
    if (drv_f1 & VIRTIO_F_VERSION_1_BIT32)
        drv_f1 = VIRTIO_F_VERSION_1_BIT32;
    else
        drv_f1 = 0;

    vw32(cc, CC_DRIVER_FEATURE_SEL, 0);
    vw32(cc, CC_DRIVER_FEATURE, drv_f0);
    vw32(cc, CC_DRIVER_FEATURE_SEL, 1);
    vw32(cc, CC_DRIVER_FEATURE, drv_f1);

    vw8(cc, CC_DEVICE_STATUS,
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    if (!(vr8(cc, CC_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        klog("[virtio-input] feature negotiation failed\n");
        return false;
    }

    if (vi_alloc_evq(s, dev) < 0)
        return false;

    size_t bytes = (size_t)s->evq.size * VI_SLOT_BYTES;
    size_t pages = (bytes + 4095) / 4096;
    s->buf_phys = pmm_alloc_pages(pages);
    if (!s->buf_phys)
        return false;
    s->buf_virt = (uint8_t *)(s->buf_phys + PHYS_MAP_BASE);
    memset(s->buf_virt, 0, pages * 4096);

    struct vi_vq *q = &s->evq;
    for (uint16_t i = 0; i < q->size; i++) {
        q->desc[i].addr  = s->buf_phys + (uint64_t)i * VI_SLOT_BYTES;
        q->desc[i].len   = VI_SLOT_BYTES;
        q->desc[i].flags = VIRTQ_DESC_F_WRITE;
        q->desc[i].next  = 0;
        q->avail->ring[i] = i;
    }
    __sync_synchronize();
    q->avail->idx = q->size;
    __sync_synchronize();
    q->last_used_idx = 0;
    vi_vq_kick(s);

    vw8(cc, CC_DEVICE_STATUS,
        VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    s->ready = true;
    return true;
}

static void vi_repost_buffer(struct vi_softc *s, uint16_t slot)
{
    struct vi_vq *q = &s->evq;
    if (slot >= q->size)
        return;
    q->desc[slot].addr  = s->buf_phys + (uint64_t)slot * VI_SLOT_BYTES;
    q->desc[slot].len   = VI_SLOT_BYTES;
    q->desc[slot].flags = VIRTQ_DESC_F_WRITE;
    q->desc[slot].next  = 0;

    uint16_t ai = q->avail->idx % q->size;
    q->avail->ring[ai] = slot;
    __sync_synchronize();
    q->avail->idx++;
    __sync_synchronize();
    vi_vq_kick(s);
}

static void vi_drain_one(struct vi_softc *s)
{
    struct vi_vq *q = &s->evq;
    __sync_synchronize();
    if ((uint16_t)(q->used->idx - q->last_used_idx) == 0)
        return;

    uint16_t ui = (uint16_t)(q->last_used_idx % q->size);
    uint32_t id = q->used->ring[ui].id;
    uint32_t ln = q->used->ring[ui].len;
    q->last_used_idx++;

    if (id >= (uint32_t)q->size) {
        s->ready = false;
        return;
    }

    uint8_t *buf = s->buf_virt + (uintptr_t)id * VI_SLOT_BYTES;
    for (uint32_t off = 0; off + 8 <= ln; off += 8) {
        uint16_t type = (uint16_t)(buf[off] | (buf[off + 1] << 8));
        uint16_t code = (uint16_t)(buf[off + 2] | (buf[off + 3] << 8));
        int32_t  val  = (int32_t)((uint32_t)buf[off + 4] | ((uint32_t)buf[off + 5] << 8) |
                                  ((uint32_t)buf[off + 6] << 16) | ((uint32_t)buf[off + 7] << 24));
        vi_handle_evdev(type, code, val);
    }

    vi_repost_buffer(s, (uint16_t)id);
}

int virtio_input_init(void)
{
    if (vi_global_init_done)
        return vi_count > 0 ? 0 : -1;
    vi_global_init_done = true;

    for (uint32_t i = 0; i < pci_get_device_count() && vi_count < VI_MAX_DEVICES; i++) {
        pci_device_t *d = pci_get_device(i);
        if (!d || !vi_is_virtio_input(d))
            continue;
        struct vi_softc *s = &vi_dev[vi_count];
        if (vi_setup_pci_device(d, s)) {
            klog("[virtio-input] device %u:%u.%u ok\n",
                 d->bus, d->device, d->function);
            vi_count++;
        }
    }

    if (vi_count == 0) {
        klog("[virtio-input] no virtio-input PCI devices\n");
        return -1;
    }
    return 0;
}

void virtio_input_poll(void)
{
    for (unsigned d = 0; d < vi_count; d++) {
        if (!vi_dev[d].ready)
            continue;
        for (unsigned n = 0; n < 64; n++)
            vi_drain_one(&vi_dev[d]);
    }
}
