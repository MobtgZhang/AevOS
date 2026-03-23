#include "block_dev.h"
#include "nvme.h"
#include "ahci.h"
#include "../klog.h"
#include <lib/string.h>

static bool (*s_read)(uint64_t lba512, uint32_t nsect, void *buf);
static bool (*s_write)(uint64_t lba512, uint32_t nsect, const void *buf);

static bool nvme_blk_read(uint64_t lba512, uint32_t nsect, void *buf)
{
    nvme_device_t *d = nvme_get_device();
    if (!d || nsect == 0 || !buf)
        return false;
    uint32_t lbs = d->ns.lba_size;
    if (lbs == 512)
        return nvme_read(lba512, nsect, buf);
    if (lbs == 4096) {
        if ((lba512 % 8u) != 0 || (nsect % 8u) != 0)
            return false;
        return nvme_read(lba512 / 8, nsect / 8, buf);
    }
    return false;
}

static bool nvme_blk_write(uint64_t lba512, uint32_t nsect, const void *buf)
{
    nvme_device_t *d = nvme_get_device();
    if (!d || nsect == 0 || !buf)
        return false;
    uint32_t lbs = d->ns.lba_size;
    if (lbs == 512)
        return nvme_write(lba512, nsect, buf);
    if (lbs == 4096) {
        if ((lba512 % 8u) != 0 || (nsect % 8u) != 0)
            return false;
        return nvme_write(lba512 / 8, nsect / 8, buf);
    }
    return false;
}

static bool ahci_blk_read(uint64_t lba512, uint32_t nsect, void *buf)
{
    return ahci_read_sectors(lba512, nsect, buf);
}

static bool ahci_blk_write(uint64_t lba512, uint32_t nsect, const void *buf)
{
    return ahci_write_sectors(lba512, nsect, buf);
}

void block_storage_register_default(void)
{
    s_read  = NULL;
    s_write = NULL;

    if (nvme_get_device()) {
        s_read  = nvme_blk_read;
        s_write = nvme_blk_write;
        klog("[blk] backend: NVMe (512B LBA or aligned 4Kn)\n");
        return;
    }
    if (ahci_is_ready()) {
        s_read  = ahci_blk_read;
        s_write = ahci_blk_write;
        klog("[blk] backend: AHCI SATA\n");
        return;
    }
    klog("[blk] no block backend (no NVMe / AHCI disk)\n");
}

bool block_dev_is_available(void)
{
    return s_read != NULL;
}

bool block_dev_read(uint64_t lba512, uint32_t num_sectors, void *buffer)
{
    if (!s_read || num_sectors == 0 || !buffer)
        return false;
    return s_read(lba512, num_sectors, buffer);
}

bool block_dev_write(uint64_t lba512, uint32_t num_sectors, const void *buffer)
{
    if (!s_write || num_sectors == 0 || !buffer)
        return false;
    return s_write(lba512, num_sectors, buffer);
}
