#include "ahci.h"
#include "pci.h"
#include "../klog.h"
#include "../mm/pmm.h"
#include <kernel/mm/vmm.h>
#include <aevos/config.h>
#include <lib/string.h>

#if !defined(__x86_64__)

bool ahci_init(void)
{
    return false;
}

bool ahci_is_ready(void)
{
    return false;
}

bool ahci_read_sectors(uint64_t lba512, uint32_t count, void *buffer)
{
    (void)lba512;
    (void)count;
    (void)buffer;
    return false;
}

bool ahci_write_sectors(uint64_t lba512, uint32_t count, const void *buffer)
{
    (void)lba512;
    (void)count;
    (void)buffer;
    return false;
}

#else /* __x86_64__ */

#define HBA_PxCMD_ST   0x0001
#define HBA_PxCMD_FRE  0x0010
#define HBA_PxCMD_FR   0x4000
#define HBA_PxCMD_CR   0x8000
#define SATA_SIG_ATA   0x00000101
#define ATA_CMD_READ_DMA_EX   0x25
#define ATA_CMD_WRITE_DMA_EX  0x35
#define AHCI_GHC_AE    (1u << 31)

typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
} hba_mem_t;

typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
} hba_port_t;

typedef struct PACKED {
    uint32_t dw0;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} hba_cmd_header_t;

typedef struct PACKED {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    uint32_t prdt_dw0;
    uint32_t prdt_dw1;
    uint32_t prdt_dw2;
    uint32_t prdt_dw3;
} ahci_cmd_table_t;

static volatile hba_mem_t  *g_hba;
static volatile hba_port_t *g_port;
static uint64_t             g_clb_phys;
static uint64_t             g_fb_phys;
static uint64_t             g_ct_phys;
static void                *g_clb_virt;
static void                *g_fb_virt;
static void                *g_ct_virt;
static bool                 g_ready;

static void port_stop(void)
{
    volatile hba_port_t *p = g_port;
    p->cmd &= ~HBA_PxCMD_ST;
    p->cmd &= ~HBA_PxCMD_FRE;
    for (int i = 0; i < 1000000; i++) {
        if (!(p->cmd & HBA_PxCMD_CR))
            return;
    }
}

static void port_start(void)
{
    volatile hba_port_t *p = g_port;
    while (p->cmd & HBA_PxCMD_CR) { }
    p->cmd |= HBA_PxCMD_FRE;
    p->cmd |= HBA_PxCMD_ST;
}

static bool ahci_do_io(uint8_t cmd_byte, uint64_t lba, uint32_t nsect,
                       void *buf, bool is_write)
{
    if (!g_ready || nsect == 0 || nsect > 256 || !buf)
        return false;

    uint64_t buf_phys = VIRT_TO_PHYS((uintptr_t)buf);
    if ((buf_phys & 1u) != 0)
        return false;

    volatile hba_port_t *p = g_port;
    while (p->tfd & 0x88) { }

    hba_cmd_header_t *hdr = (hba_cmd_header_t *)g_clb_virt;
    memset((void *)hdr, 0, sizeof(*hdr));
    uint32_t dw0 = (1u << 16) | (16u & 0x1Fu);
    if (is_write)
        dw0 |= (1u << 6);
    hdr->dw0   = dw0;
    hdr->ctba  = (uint32_t)g_ct_phys;
    hdr->ctbau = (uint32_t)(g_ct_phys >> 32);

    ahci_cmd_table_t *ct = (ahci_cmd_table_t *)g_ct_virt;
    memset(ct, 0, sizeof(*ct));
    uint8_t *f = ct->cfis;
    f[0]  = 0x27;
    f[1]  = 0x80;
    f[2]  = cmd_byte;
    f[3]  = 0;
    f[4]  = (uint8_t)(lba & 0xFF);
    f[5]  = (uint8_t)((lba >> 8) & 0xFF);
    f[6]  = (uint8_t)((lba >> 16) & 0xFF);
    f[7]  = (uint8_t)(1u << 6);
    f[8]  = (uint8_t)((lba >> 24) & 0xFF);
    f[9]  = (uint8_t)((lba >> 32) & 0xFF);
    f[10] = (uint8_t)((lba >> 40) & 0xFF);
    f[11] = 0;
    f[12] = (uint8_t)(nsect & 0xFF);
    f[13] = (uint8_t)((nsect >> 8) & 0xFF);

    uint32_t bcnt = nsect * 512u - 1u;
    ct->prdt_dw0 = (uint32_t)buf_phys;
    ct->prdt_dw1 = (uint32_t)(buf_phys >> 32);
    ct->prdt_dw2 = 0;
    ct->prdt_dw3 = bcnt | (1u << 31);

    p->ci = 1u;
    for (int spin = 0; spin < 5000000; spin++) {
        if ((p->ci & 1u) == 0)
            goto done;
    }
    klog("[ahci] command timeout (ci)\n");
    return false;

done:
    if (p->is & (1u << 30)) {
        klog("[ahci] task file error\n");
        p->is = (1u << 30);
        return false;
    }
    return true;
}

bool ahci_read_sectors(uint64_t lba512, uint32_t count, void *buffer)
{
    uint8_t *b = (uint8_t *)buffer;
    while (count > 0) {
        uint32_t chunk = count > 128 ? 128 : count;
        if (!ahci_do_io(ATA_CMD_READ_DMA_EX, lba512, chunk, b, false))
            return false;
        b += chunk * 512u;
        lba512 += chunk;
        count -= chunk;
    }
    return true;
}

bool ahci_write_sectors(uint64_t lba512, uint32_t count, const void *buffer)
{
    const uint8_t *b = (const uint8_t *)buffer;
    while (count > 0) {
        uint32_t chunk = count > 128 ? 128 : count;
        if (!ahci_do_io(ATA_CMD_WRITE_DMA_EX, lba512, chunk, (void *)b, true))
            return false;
        b += chunk * 512u;
        lba512 += chunk;
        count -= chunk;
    }
    return true;
}

bool ahci_is_ready(void)
{
    return g_ready;
}

bool ahci_init(void)
{
    g_ready = false;
    g_hba   = NULL;
    g_port  = NULL;

    pci_device_t *pdev = pci_find_class(0x01, 0x06);
    if (!pdev || pdev->prog_if != 0x01) {
        klog("[ahci] no AHCI controller (class 01:06 if=01)\n");
        return false;
    }

    uint32_t cmd = pci_read_config(pdev->bus, pdev->device, pdev->function,
                                   PCI_CFG_COMMAND);
    cmd |= (1u << 1) | (1u << 2);
    pci_write_config(pdev->bus, pdev->device, pdev->function,
                     PCI_CFG_COMMAND, cmd);

    uint64_t abar = pci_get_bar(pdev, 5);
    if (!abar)
        abar = pci_get_bar(pdev, 0);
    if (!abar) {
        klog("[ahci] ABAR missing\n");
        return false;
    }

    g_hba = (volatile hba_mem_t *)PHYS_TO_VIRT(abar);
    g_hba->ghc |= AHCI_GHC_AE;

    if ((g_hba->pi & 1u) == 0) {
        klog("[ahci] port 0 not implemented (pi=%x)\n", g_hba->pi);
        return false;
    }

    g_port = (volatile hba_port_t *)((uint8_t *)g_hba + 0x100);

    g_clb_phys = pmm_alloc_page();
    g_fb_phys  = pmm_alloc_page();
    g_ct_phys  = pmm_alloc_page();
    if (!g_clb_phys || !g_fb_phys || !g_ct_phys) {
        klog("[ahci] DMA page alloc failed\n");
        return false;
    }

    g_clb_virt = PHYS_TO_VIRT(g_clb_phys);
    g_fb_virt  = PHYS_TO_VIRT(g_fb_phys);
    g_ct_virt  = PHYS_TO_VIRT(g_ct_phys);
    memset(g_clb_virt, 0, PAGE_SIZE);
    memset(g_fb_virt, 0, PAGE_SIZE);
    memset(g_ct_virt, 0, PAGE_SIZE);

    port_stop();
    g_port->clb  = (uint32_t)g_clb_phys;
    g_port->clbu = (uint32_t)(g_clb_phys >> 32);
    g_port->fb   = (uint32_t)g_fb_phys;
    g_port->fbu  = (uint32_t)(g_fb_phys >> 32);
    port_start();

    for (int w = 0; w < 1000000; w++) {
        if (g_port->sig == SATA_SIG_ATA)
            break;
    }
    if (g_port->sig != SATA_SIG_ATA) {
        klog("[ahci] port0 sig=%08x (expected %08x), no disk?\n",
             g_port->sig, SATA_SIG_ATA);
        port_stop();
        return false;
    }

    g_ready = true;
    klog("[ahci] port0 ready (SATA ATA signature)\n");
    return true;
}

#endif /* __x86_64__ */
