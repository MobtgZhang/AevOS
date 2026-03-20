#include "gdt.h"

static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdt_ptr;
static tss_t       tss;

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    gdt[idx].base_low    = base & 0xFFFF;
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].base_high   = (base >> 24) & 0xFF;
    gdt[idx].limit_low   = limit & 0xFFFF;
    gdt[idx].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[idx].access      = access;
}

static void gdt_set_tss(void) {
    uint64_t base  = (uint64_t)&tss;
    uint32_t limit = sizeof(tss) - 1;

    tss_desc_t *desc = (tss_desc_t *)&gdt[GDT_TSS];

    desc->limit_low  = limit & 0xFFFF;
    desc->base_low   = base & 0xFFFF;
    desc->base_mid   = (base >> 16) & 0xFF;
    desc->access     = 0x89;        /* present, 64-bit TSS available */
    desc->flags_limit = (limit >> 16) & 0x0F;
    desc->base_mid2  = (base >> 24) & 0xFF;
    desc->base_high  = (base >> 32) & 0xFFFFFFFF;
    desc->reserved   = 0;
}

void gdt_init(void) {
    /* Null descriptor */
    gdt_set_entry(GDT_NULL, 0, 0, 0, 0);

    /* Kernel Code 64-bit: execute/read, DPL=0, long mode */
    gdt_set_entry(GDT_KCODE, 0, 0xFFFFF, 0x9A, 0x20);

    /* Kernel Data: read/write, DPL=0 */
    gdt_set_entry(GDT_KDATA, 0, 0xFFFFF, 0x92, 0x00);

    /* User Data: read/write, DPL=3 (before user code for sysret) */
    gdt_set_entry(GDT_UDATA, 0, 0xFFFFF, 0xF2, 0x00);

    /* User Code 64-bit: execute/read, DPL=3, long mode */
    gdt_set_entry(GDT_UCODE, 0, 0xFFFFF, 0xFA, 0x20);

    /* Clear TSS structure */
    for (size_t i = 0; i < sizeof(tss); i++)
        ((uint8_t *)&tss)[i] = 0;

    tss.iopb_offset = sizeof(tss);

    /* TSS descriptor (occupies two GDT slots) */
    gdt_set_tss();

    /* Load GDT */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;
    gdt_flush((uint64_t)&gdt_ptr);

    /* Load TSS */
    __asm__ volatile(
        "mov %0, %%ax\n\t"
        "ltr %%ax"
        :: "i"(SEL_TSS) : "ax"
    );
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
