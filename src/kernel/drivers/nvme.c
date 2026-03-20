#include "nvme.h"
#include "pci.h"
#include "../klog.h"
#include "../mm/slab.h"
#include <aevos/config.h>

static nvme_device_t nvme_dev;

/* ── MMIO helpers ── */

static inline uint32_t nvme_read32(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void nvme_write32(volatile uint8_t *base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

static inline uint64_t nvme_read64(volatile uint8_t *base, uint32_t off)
{
    uint32_t lo = nvme_read32(base, off);
    uint32_t hi = nvme_read32(base, off + 4);
    return ((uint64_t)hi << 32) | lo;
}

static inline void nvme_write64(volatile uint8_t *base, uint32_t off, uint64_t val)
{
    nvme_write32(base, off, (uint32_t)val);
    nvme_write32(base, off + 4, (uint32_t)(val >> 32));
}

nvme_device_t *nvme_get_device(void)
{
    return nvme_dev.initialized ? &nvme_dev : NULL;
}

/* ── Queue management ── */

static void nvme_submit_cmd(nvme_queue_t *q, nvme_cmd_t *cmd)
{
    cmd->command_id = q->cid++;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (q->sq_tail + 1) % q->size;
    *q->sq_doorbell = q->sq_tail;
}

static bool nvme_poll_cpl(nvme_queue_t *q, nvme_cpl_t *out, uint32_t timeout_ms)
{
    /*
     * Spin-poll CQ until the phase bit flips, meaning a new
     * completion has been posted by the controller.
     */
    for (uint32_t i = 0; i < timeout_ms * 1000; i++) {
        nvme_cpl_t *cpl = &q->cq[q->cq_head];
        /* Phase bit is bit 0 of the status word */
        if ((cpl->status & 1) == q->cq_phase) {
            if (out) *out = *cpl;
            q->cq_head = (q->cq_head + 1) % q->size;
            if (q->cq_head == 0)
                q->cq_phase ^= 1;
            *q->cq_doorbell = q->cq_head;
            /* Check status (bits 1-15 = status code) */
            return ((cpl->status >> 1) & 0x7FFF) == 0;
        }
        /* ~1 µs spin */
        for (volatile int d = 0; d < 100; d++);
    }
    klog("[nvme] command timeout\n");
    return false;
}

static bool nvme_admin_cmd(nvme_cmd_t *cmd)
{
    nvme_submit_cmd(&nvme_dev.admin_queue, cmd);
    nvme_cpl_t cpl;
    return nvme_poll_cpl(&nvme_dev.admin_queue, &cpl, 5000);
}

/* ── Queue initialization helpers ── */

static bool init_queue(nvme_queue_t *q, uint16_t size, uint32_t sq_db_off, uint32_t cq_db_off)
{
    q->size = size;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->cq_phase = 1;
    q->cid = 0;

    q->sq = (nvme_cmd_t *)kcalloc(size, sizeof(nvme_cmd_t));
    q->cq = (nvme_cpl_t *)kcalloc(size, sizeof(nvme_cpl_t));
    if (!q->sq || !q->cq) {
        klog("[nvme] failed to allocate queue buffers\n");
        return false;
    }

    q->sq_doorbell = (volatile uint32_t *)(nvme_dev.base + sq_db_off);
    q->cq_doorbell = (volatile uint32_t *)(nvme_dev.base + cq_db_off);
    return true;
}

/* ── Initialization ── */

bool nvme_init(void)
{
    nvme_dev.initialized = false;

    pci_device_t *pdev = pci_find_class(NVME_CLASS, NVME_SUBCLASS);
    if (!pdev) {
        klog("[nvme] no NVMe controller found\n");
        return false;
    }

    klog("[nvme] found controller at PCI %u:%u.%u (vendor=%x device=%x)\n",
         pdev->bus, pdev->device, pdev->function,
         pdev->vendor_id, pdev->device_id);

    /* Enable bus mastering and memory space */
    uint32_t cmd = pci_read_config(pdev->bus, pdev->device, pdev->function, PCI_CFG_COMMAND);
    cmd |= (1 << 1) | (1 << 2);  /* Memory Space + Bus Master */
    pci_write_config(pdev->bus, pdev->device, pdev->function, PCI_CFG_COMMAND, cmd);

    nvme_dev.base = (volatile uint8_t *)(uintptr_t)pci_get_bar(pdev, 0);
    if (!nvme_dev.base) {
        klog("[nvme] BAR0 is null\n");
        return false;
    }

    /* Read capabilities */
    uint64_t cap = nvme_read64(nvme_dev.base, NVME_REG_CAP);
    nvme_dev.doorbell_stride = 4 << ((cap >> 32) & 0xF);

    klog("[nvme] controller version %x, doorbell stride %u\n",
         nvme_read32(nvme_dev.base, NVME_REG_VS), nvme_dev.doorbell_stride);

    /* Disable controller */
    nvme_write32(nvme_dev.base, NVME_REG_CC, 0);
    /* Wait for not ready */
    for (int i = 0; i < 1000000; i++) {
        if (!(nvme_read32(nvme_dev.base, NVME_REG_CSTS) & NVME_CSTS_RDY))
            break;
    }

    /* Set up admin queue */
    uint32_t sq_db = NVME_REG_SQ0TDBL;
    uint32_t cq_db = NVME_REG_SQ0TDBL + nvme_dev.doorbell_stride;
    if (!init_queue(&nvme_dev.admin_queue, NVME_ADMIN_QUEUE_SIZE, sq_db, cq_db))
        return false;

    /* Tell controller about admin queues */
    nvme_write32(nvme_dev.base, NVME_REG_AQA,
                 ((NVME_ADMIN_QUEUE_SIZE - 1) << 16) | (NVME_ADMIN_QUEUE_SIZE - 1));
    nvme_write64(nvme_dev.base, NVME_REG_ASQ, (uint64_t)(uintptr_t)nvme_dev.admin_queue.sq);
    nvme_write64(nvme_dev.base, NVME_REG_ACQ, (uint64_t)(uintptr_t)nvme_dev.admin_queue.cq);

    /* Enable controller */
    uint32_t cc = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
                  NVME_CC_AMS_RR | NVME_CC_SHN_NONE | NVME_CC_IOSQES | NVME_CC_IOCQES;
    nvme_write32(nvme_dev.base, NVME_REG_CC, cc);

    /* Wait for ready */
    for (int i = 0; i < 1000000; i++) {
        if (nvme_read32(nvme_dev.base, NVME_REG_CSTS) & NVME_CSTS_RDY)
            goto ready;
    }
    klog("[nvme] controller not ready\n");
    return false;

ready:
    klog("[nvme] controller enabled\n");

    if (!nvme_identify()) return false;

    /* Create I/O completion queue (ID=1) */
    uint32_t io_cq_db = NVME_REG_SQ0TDBL + 3 * nvme_dev.doorbell_stride;
    uint32_t io_sq_db = NVME_REG_SQ0TDBL + 2 * nvme_dev.doorbell_stride;

    nvme_dev.io_queue.size = NVME_IO_QUEUE_SIZE;
    nvme_dev.io_queue.sq_tail = 0;
    nvme_dev.io_queue.cq_head = 0;
    nvme_dev.io_queue.cq_phase = 1;
    nvme_dev.io_queue.cid = 0;
    nvme_dev.io_queue.sq = (nvme_cmd_t *)kcalloc(NVME_IO_QUEUE_SIZE, sizeof(nvme_cmd_t));
    nvme_dev.io_queue.cq = (nvme_cpl_t *)kcalloc(NVME_IO_QUEUE_SIZE, sizeof(nvme_cpl_t));
    if (!nvme_dev.io_queue.sq || !nvme_dev.io_queue.cq) return false;
    nvme_dev.io_queue.sq_doorbell = (volatile uint32_t *)(nvme_dev.base + io_sq_db);
    nvme_dev.io_queue.cq_doorbell = (volatile uint32_t *)(nvme_dev.base + io_cq_db);

    /* Admin command: Create I/O CQ */
    nvme_cmd_t ncmd;
    for (int i = 0; i < 16; i++) ((uint32_t *)&ncmd)[i] = 0;

    ncmd.opcode = NVME_ADMIN_CREATE_IOCQ;
    ncmd.prp1   = (uint64_t)(uintptr_t)nvme_dev.io_queue.cq;
    ncmd.cdw10  = ((NVME_IO_QUEUE_SIZE - 1) << 16) | 1; /* QID=1 */
    ncmd.cdw11  = (1 << 0); /* physically contiguous */
    if (!nvme_admin_cmd(&ncmd)) {
        klog("[nvme] create I/O CQ failed\n");
        return false;
    }

    /* Admin command: Create I/O SQ */
    for (int i = 0; i < 16; i++) ((uint32_t *)&ncmd)[i] = 0;
    ncmd.opcode = NVME_ADMIN_CREATE_IOSQ;
    ncmd.prp1   = (uint64_t)(uintptr_t)nvme_dev.io_queue.sq;
    ncmd.cdw10  = ((NVME_IO_QUEUE_SIZE - 1) << 16) | 1; /* QID=1 */
    ncmd.cdw11  = (1 << 16) | (1 << 0); /* CQID=1, physically contiguous */
    if (!nvme_admin_cmd(&ncmd)) {
        klog("[nvme] create I/O SQ failed\n");
        return false;
    }

    nvme_dev.initialized = true;
    klog("[nvme] driver initialized: ns1 size=%llu LBAs, lba_size=%u\n",
         nvme_dev.ns.size, nvme_dev.ns.lba_size);
    return true;
}

bool nvme_identify(void)
{
    /* Identify Controller */
    uint8_t *identify_buf = (uint8_t *)kcalloc(1, 4096);
    if (!identify_buf) return false;

    nvme_cmd_t cmd;
    for (int i = 0; i < 16; i++) ((uint32_t *)&cmd)[i] = 0;
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid   = 0;
    cmd.prp1   = (uint64_t)(uintptr_t)identify_buf;
    cmd.cdw10  = 1; /* CNS = Identify Controller */

    if (!nvme_admin_cmd(&cmd)) {
        klog("[nvme] identify controller failed\n");
        kfree(identify_buf);
        return false;
    }

    klog("[nvme] controller identified\n");

    /* Identify Namespace 1 */
    for (int i = 0; i < 16; i++) ((uint32_t *)&cmd)[i] = 0;
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid   = 1;
    cmd.prp1   = (uint64_t)(uintptr_t)identify_buf;
    cmd.cdw10  = 0; /* CNS = Identify Namespace */

    if (!nvme_admin_cmd(&cmd)) {
        klog("[nvme] identify namespace 1 failed\n");
        kfree(identify_buf);
        return false;
    }

    /* Parse namespace data */
    nvme_dev.ns.nsid = 1;
    nvme_dev.ns.size     = *(uint64_t *)(identify_buf + 0);   /* NSZE */
    nvme_dev.ns.capacity = *(uint64_t *)(identify_buf + 8);   /* NCAP */

    /* LBA format: byte 128+, formatted LBA size from FLBAS */
    uint8_t flbas = identify_buf[26];
    uint8_t lba_fmt_idx = flbas & 0x0F;
    /* LBA format descriptors start at offset 128 */
    uint32_t lba_fmt = *(uint32_t *)(identify_buf + 128 + lba_fmt_idx * 4);
    uint8_t lba_ds = (lba_fmt >> 16) & 0xFF;
    nvme_dev.ns.lba_size = (lba_ds > 0) ? (1u << lba_ds) : 512;

    kfree(identify_buf);
    klog("[nvme] ns1: size=%llu capacity=%llu lba_size=%u\n",
         nvme_dev.ns.size, nvme_dev.ns.capacity, nvme_dev.ns.lba_size);
    return true;
}

/* ── I/O operations ── */

static bool nvme_io_cmd(uint8_t opcode, uint64_t lba, uint32_t count, void *buffer)
{
    if (!nvme_dev.initialized || count == 0 || !buffer)
        return false;

    nvme_cmd_t cmd;
    for (int i = 0; i < 16; i++) ((uint32_t *)&cmd)[i] = 0;

    cmd.opcode = opcode;
    cmd.nsid   = nvme_dev.ns.nsid;
    cmd.prp1   = (uint64_t)(uintptr_t)buffer;

    /*
     * For transfers > 1 page, PRP2 points to a PRP list.
     * For simplicity, handle up to 2 pages (8 KB) inline;
     * larger transfers would need a PRP list allocation.
     */
    size_t xfer_bytes = (size_t)count * nvme_dev.ns.lba_size;
    uint64_t prp1_end = (cmd.prp1 & ~0xFFFULL) + PAGE_SIZE;
    if ((cmd.prp1 + xfer_bytes) > prp1_end) {
        cmd.prp2 = prp1_end;
    }

    cmd.cdw10 = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;  /* 0-based count */

    nvme_submit_cmd(&nvme_dev.io_queue, &cmd);
    nvme_cpl_t cpl;
    return nvme_poll_cpl(&nvme_dev.io_queue, &cpl, 5000);
}

bool nvme_read(uint64_t lba, uint32_t count, void *buffer)
{
    return nvme_io_cmd(NVME_IO_CMD_READ, lba, count, buffer);
}

bool nvme_write(uint64_t lba, uint32_t count, const void *buffer)
{
    return nvme_io_cmd(NVME_IO_CMD_WRITE, lba, count, (void *)buffer);
}
