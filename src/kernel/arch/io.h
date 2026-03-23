#pragma once

/*
 * Architecture-specific I/O and CPU control primitives.
 *
 * x86-64:      port I/O (in/out), CR register access, TLB flush
 * aarch64:     MMIO, system registers, TLB invalidation
 * riscv64:     MMIO, CSR access, SFENCE.VMA
 * loongarch64: MMIO, CSR access, INVTLB
 */

#include <aevos/types.h>

/* ── MMIO (common to all architectures) ──────────────────── */

static inline uint8_t mmio_read8(volatile void *addr) {
    return *(volatile uint8_t *)addr;
}
static inline void mmio_write8(volatile void *addr, uint8_t val) {
    *(volatile uint8_t *)addr = val;
}
static inline uint32_t mmio_read32(volatile void *addr) {
    return *(volatile uint32_t *)addr;
}
static inline void mmio_write32(volatile void *addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}
static inline uint64_t mmio_read64(volatile void *addr) {
    return *(volatile uint64_t *)addr;
}
static inline void mmio_write64(volatile void *addr, uint64_t val) {
    *(volatile uint64_t *)addr = val;
}

/* ── x86-64 ──────────────────────────────────────────────── */
#if defined(__x86_64__)

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }

static inline void invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}
static inline void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}
static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}
static inline void io_wait(void) { outb(0x80, 0); }

/* ── aarch64 ─────────────────────────────────────────────── */
#elif defined(__aarch64__)

static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline void outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0; }
static inline void outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
static inline uint32_t inl(uint16_t port) { (void)port; return 0; }

static inline void cli(void) { __asm__ volatile("msr daifset, #15"); }
static inline void sti(void) { __asm__ volatile("msr daifclr, #15"); }

static inline void invlpg(uint64_t addr) {
    __asm__ volatile("tlbi vaae1is, %0\n\tdsb sy\n\tisb" : : "r"(addr >> 12));
}
static inline void write_cr3(uint64_t val) {
    __asm__ volatile(
        "msr ttbr1_el1, %0\n\t"
        "isb\n\t"
        "tlbi vmalle1\n\t"
        "dsb sy\n\t"
        "isb"
        : : "r"(val) : "memory"
    );
}
static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(val));
    return val;
}
static inline void io_wait(void) { __asm__ volatile("nop"); }

/* ── riscv64 ─────────────────────────────────────────────── */
#elif defined(__riscv)

static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline void outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0; }
static inline void outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
static inline uint32_t inl(uint16_t port) { (void)port; return 0; }

static inline void cli(void) { __asm__ volatile("csrci mstatus, 8"); }
static inline void sti(void) { __asm__ volatile("csrsi mstatus, 8"); }

static inline void invlpg(uint64_t addr) {
    __asm__ volatile("sfence.vma %0, zero" : : "r"(addr));
}
static inline void write_cr3(uint64_t val) {
    /* Sv48 (mode 9): 与 kernel.lds / PHYS_MAP_BASE 高半部布局一致；Sv39 无法覆盖该 VA 空间 */
    uint64_t satp = (9ULL << 60) | ((val >> 12) & 0x00000FFFFFFFFFFFULL);
    __asm__ volatile("csrw satp, %0\n\tsfence.vma zero, zero" : : "r"(satp) : "memory");
}
static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, satp" : "=r"(val));
    return (val & 0x00000FFFFFFFFFFFULL) << 12;
}
static inline void io_wait(void) { __asm__ volatile("nop"); }

/* ── loongarch64 ─────────────────────────────────────────── */
#elif defined(__loongarch64)

static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline void outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0; }
static inline void outl(uint16_t port, uint32_t val) { (void)port; (void)val; }
static inline uint32_t inl(uint16_t port) { (void)port; return 0; }

static inline void cli(void) { __asm__ volatile("csrwr $zero, 0x4"); }
static inline void sti(void) {
    uint64_t val;
    __asm__ volatile("csrrd %0, 0x4" : "=r"(val));
    val |= 0xFUL;
    __asm__ volatile("csrwr %0, 0x4" : : "r"(val));
}

static inline void invlpg(uint64_t addr) {
    __asm__ volatile("invtlb 0x5, $zero, %0" : : "r"(addr));
}
static inline void write_cr3(uint64_t val) {
    __asm__ volatile("csrwr %0, 0x19" : : "r"(val));
}
static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("csrrd %0, 0x19" : "=r"(val));
    return val;
}
static inline void io_wait(void) { __asm__ volatile("nop"); }

#else
#error "Unsupported architecture for I/O"
#endif
