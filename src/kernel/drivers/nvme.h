#pragma once

#include <aevos/types.h>

/* ── NVMe constants ── */

#define NVME_CLASS          0x01
#define NVME_SUBCLASS       0x08
#define NVME_PROG_IF        0x02

#define NVME_ADMIN_QUEUE_SIZE  64
#define NVME_IO_QUEUE_SIZE    256
#define NVME_SECTOR_SIZE      512

/* NVMe controller register offsets */
#define NVME_REG_CAP     0x00
#define NVME_REG_VS      0x08
#define NVME_REG_INTMS   0x0C
#define NVME_REG_INTMC   0x10
#define NVME_REG_CC      0x14
#define NVME_REG_CSTS    0x1C
#define NVME_REG_AQA     0x24
#define NVME_REG_ASQ     0x28
#define NVME_REG_ACQ     0x30
#define NVME_REG_SQ0TDBL 0x1000

/* CC register bits */
#define NVME_CC_EN       (1 << 0)
#define NVME_CC_CSS_NVM  (0 << 4)
#define NVME_CC_MPS_4K   (0 << 7)
#define NVME_CC_AMS_RR   (0 << 11)
#define NVME_CC_SHN_NONE (0 << 14)
#define NVME_CC_IOSQES   (6 << 16)
#define NVME_CC_IOCQES   (4 << 20)

/* CSTS register bits */
#define NVME_CSTS_RDY    (1 << 0)

/* NVMe opcodes */
#define NVME_ADMIN_DELETE_IOSQ  0x00
#define NVME_ADMIN_CREATE_IOSQ  0x01
#define NVME_ADMIN_DELETE_IOCQ  0x04
#define NVME_ADMIN_CREATE_IOCQ  0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_SET_FEATURES 0x09

#define NVME_IO_CMD_READ   0x02
#define NVME_IO_CMD_WRITE  0x01

/* ── NVMe command (64 bytes) ── */

typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} PACKED nvme_cmd_t;

/* ── NVMe completion entry (16 bytes) ── */

typedef struct {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
} PACKED nvme_cpl_t;

/* ── Queue pair ── */

typedef struct {
    nvme_cmd_t *sq;
    nvme_cpl_t *cq;
    volatile uint32_t *sq_doorbell;
    volatile uint32_t *cq_doorbell;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint16_t size;
    uint8_t  cq_phase;
    uint16_t cid;
} nvme_queue_t;

/* ── Namespace info ── */

typedef struct {
    uint64_t size;
    uint64_t capacity;
    uint32_t lba_size;
    uint32_t nsid;
} nvme_ns_info_t;

/* ── Device ── */

typedef struct {
    volatile uint8_t *base;
    uint32_t          doorbell_stride;
    nvme_queue_t      admin_queue;
    nvme_queue_t      io_queue;
    nvme_ns_info_t    ns;
    bool              initialized;
} nvme_device_t;

/* ── API ── */

bool nvme_init(void);
bool nvme_identify(void);
bool nvme_read(uint64_t lba, uint32_t count, void *buffer);
bool nvme_write(uint64_t lba, uint32_t count, const void *buffer);
nvme_device_t *nvme_get_device(void);
