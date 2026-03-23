#include "pci.h"
#include "../arch/io.h"
#include "../klog.h"
#include <aevos/config.h>

static pci_device_t devices[PCI_MAX_DEVICES];
static uint32_t     device_count = 0;

/* ── config-space I/O ── */

#if PCI_USE_ECAM

/*
 * ECAM (Enhanced Configuration Access Mechanism) for non-x86 platforms.
 * PCI config space is memory-mapped at PCI_ECAM_BASE:
 *   address = ECAM_BASE + (bus << 20) + (dev << 15) + (func << 12) + offset
 */

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    volatile uint32_t *addr = (volatile uint32_t *)(
        PCI_ECAM_BASE +
        ((uint64_t)bus  << 20) +
        ((uint64_t)(dev & 0x1F)  << 15) +
        ((uint64_t)(func & 0x07) << 12) +
        (offset & 0xFC)
    );
    return *addr;
}

void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value)
{
    volatile uint32_t *addr = (volatile uint32_t *)(
        PCI_ECAM_BASE +
        ((uint64_t)bus  << 20) +
        ((uint64_t)(dev & 0x1F)  << 15) +
        ((uint64_t)(func & 0x07) << 12) +
        (offset & 0xFC)
    );
    *addr = value;
}

uint8_t pci_read_config8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    volatile uint8_t *addr = (volatile uint8_t *)(
        PCI_ECAM_BASE +
        ((uint64_t)bus  << 20) +
        ((uint64_t)(dev & 0x1F)  << 15) +
        ((uint64_t)(func & 0x07) << 12) +
        offset
    );
    return *addr;
}

uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    volatile uint16_t *addr = (volatile uint16_t *)(
        PCI_ECAM_BASE +
        ((uint64_t)bus  << 20) +
        ((uint64_t)(dev & 0x1F)  << 15) +
        ((uint64_t)(func & 0x07) << 12) +
        (offset & ~1u)
    );
    return *addr;
}

#else /* x86 port I/O */

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)(dev & 0x1F) << 11)
                  | ((uint32_t)(func & 0x07) << 8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)(dev & 0x1F) << 11)
                  | ((uint32_t)(func & 0x07) << 8)
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, value);
}

uint8_t pci_read_config8(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    uint32_t val = pci_read_config(bus, dev, func, (uint8_t)(offset & 0xFC));
    return (uint8_t)(val >> ((offset & 3) * 8));
}

uint16_t pci_read_config16(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset)
{
    uint32_t val = pci_read_config(bus, dev, func, (uint8_t)(offset & 0xFC));
    return (uint16_t)(val >> ((offset & 2) * 8));
}

#endif /* PCI_USE_ECAM */

/* ── BAR decoding ── */

static uint64_t decode_bar(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_offset)
{
    uint32_t bar_lo = pci_read_config(bus, dev, func, bar_offset);

    if (bar_lo & PCI_BAR_IO) {
        return bar_lo & ~0x3u;
    }

    uint64_t addr = bar_lo & ~0xFu;

    if ((bar_lo & 0x06) == PCI_BAR_MEM_64) {
        uint32_t bar_hi = pci_read_config(bus, dev, func, bar_offset + 4);
        addr |= (uint64_t)bar_hi << 32;
    }

#if defined(__loongarch64)
    if (addr)
        addr += LOONGARCH_MMIO_BASE;
#elif defined(__aarch64__)
    if (addr)
        addr += AARCH64_MMIO_BASE;
#elif defined(__riscv)
    if (addr)
        addr += PHYS_MAP_BASE;
#endif

    return addr;
}

/* ── enumeration ── */

static void probe_function(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t reg0 = pci_read_config(bus, dev, func, PCI_CFG_VENDOR_ID);
    uint16_t vendor = reg0 & 0xFFFF;
    if (vendor == PCI_VENDOR_NONE || vendor == 0x0000)
        return;

    if (device_count >= PCI_MAX_DEVICES) return;

    pci_device_t *d = &devices[device_count];
    d->bus        = bus;
    d->device     = dev;
    d->function   = func;
    d->vendor_id  = vendor;
    d->device_id  = (reg0 >> 16) & 0xFFFF;

    uint32_t reg2 = pci_read_config(bus, dev, func, PCI_CFG_REVISION);
    d->revision   = reg2 & 0xFF;
    d->prog_if    = (reg2 >> 8) & 0xFF;
    d->subclass   = (reg2 >> 16) & 0xFF;
    d->class_code = (reg2 >> 24) & 0xFF;

    uint32_t reg3 = pci_read_config(bus, dev, func, PCI_CFG_HEADER_TYPE);
    d->header_type = (reg3 >> 16) & 0xFF;

    uint32_t irq_reg = pci_read_config(bus, dev, func, PCI_CFG_INT_LINE);
    d->irq = irq_reg & 0xFF;

    for (int i = 0; i < 6; i++) {
        uint8_t bar_off = PCI_CFG_BAR0 + (uint8_t)(i * 4);
        d->bar[i] = decode_bar(bus, dev, func, bar_off);
        uint32_t bar_raw = pci_read_config(bus, dev, func, bar_off);
        if (!(bar_raw & PCI_BAR_IO) && (bar_raw & 0x06) == PCI_BAR_MEM_64) {
            i++;
            if (i < 6) d->bar[i] = 0;
        }
    }

    klog("[pci] %u:%u.%u vendor=%x device=%x class=%x:%x\n",
         bus, dev, func, d->vendor_id, d->device_id, d->class_code, d->subclass);
    device_count++;
}

static void probe_device(uint8_t bus, uint8_t dev)
{
    uint32_t reg0 = pci_read_config(bus, dev, 0, PCI_CFG_VENDOR_ID);
    if ((reg0 & 0xFFFF) == PCI_VENDOR_NONE)
        return;

    probe_function(bus, dev, 0);

    uint32_t hdr = pci_read_config(bus, dev, 0, PCI_CFG_HEADER_TYPE);
    if ((hdr >> 16) & 0x80) {
        for (uint8_t func = 1; func < PCI_MAX_FUNC; func++)
            probe_function(bus, dev, func);
    }
}

void pci_init(void)
{
    device_count = 0;
    klog("[pci] enumerating PCI buses...\n");

#ifdef PCI_ECAM_BUS_LIMIT
    uint32_t max_bus = PCI_ECAM_BUS_LIMIT;
#else
    uint32_t max_bus = PCI_MAX_BUS;
#endif

    for (uint32_t bus = 0; bus < max_bus; bus++) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEV; dev++) {
            probe_device((uint8_t)bus, dev);
        }
    }

    klog("[pci] found %u devices\n", device_count);
}

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id)
{
    for (uint32_t i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor_id && devices[i].device_id == device_id)
            return &devices[i];
    }
    return NULL;
}

pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (uint32_t i = 0; i < device_count; i++) {
        if (devices[i].class_code == class_code && devices[i].subclass == subclass)
            return &devices[i];
    }
    return NULL;
}

uint64_t pci_get_bar(pci_device_t *dev, uint8_t bar_index)
{
    if (!dev || bar_index >= 6) return 0;
    return dev->bar[bar_index];
}

uint32_t pci_get_device_count(void)
{
    return device_count;
}

pci_device_t *pci_get_device(uint32_t index)
{
    if (index >= device_count) return NULL;
    return &devices[index];
}

void *pci_bar_to_mmio_vaddr(uint64_t bar_phys)
{
#if defined(__x86_64__)
    if (bar_phys < 0x100000000ULL)
        return (void *)(uintptr_t)bar_phys;
    if (bar_phys >= AEVOS_X86_PCI_MMIO64_PHYS_LO &&
        bar_phys < AEVOS_X86_PCI_MMIO64_PHYS_LO + AEVOS_X86_PCI_MMIO64_PHYS_SZ)
        return (void *)(uintptr_t)(PHYS_MAP_BASE + bar_phys);
    klog("[pci] MMIO BAR phys %llx outside high MMIO window (extend SZ in pci.h)\n",
         (unsigned long long)bar_phys);
    return NULL;
#else
    return (void *)(uintptr_t)bar_phys;
#endif
}
