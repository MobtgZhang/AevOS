#include "serial.h"
#include "../arch/io.h"
#include <aevos/config.h>

/* UART 16550 register offsets */
#define UART_DATA       0   /* RBR / THR */
#define UART_IER        1   /* Interrupt Enable */
#define UART_FCR        2   /* FIFO Control (write) / IIR (read) */
#define UART_LCR        3   /* Line Control */
#define UART_MCR        4   /* Modem Control */
#define UART_LSR        5   /* Line Status */
#define UART_MSR        6   /* Modem Status */
#define UART_SCRATCH    7

#define UART_LSR_TX_EMPTY 0x20
#define UART_LSR_RX_READY 0x01

#define UART_LCR_DLAB    0x80
#define UART_LCR_8N1     0x03

#define UART_FCR_ENABLE  0x01
#define UART_FCR_CLR_RX  0x02
#define UART_FCR_CLR_TX  0x04
#define UART_FCR_TRIG_14 0xC0

#define UART_MCR_DTR     0x01
#define UART_MCR_RTS     0x02
#define UART_MCR_OUT2    0x08

static bool uart_ready = false;

#if SERIAL_USE_PIO

/* ── x86: Port I/O UART ──────────────────────────────── */
static uint16_t uart_port = 0;

static inline void uart_out(uint16_t reg, uint8_t val)
{
    outb(uart_port + reg, val);
}

static inline uint8_t uart_in(uint16_t reg)
{
    return inb(uart_port + reg);
}

void serial_init(uint16_t port)
{
    uart_port = port;
    uart_ready = false;

    uart_out(UART_IER, 0x00);

    uart_out(UART_LCR, UART_LCR_DLAB);
    uart_out(UART_DATA, 0x01);
    uart_out(UART_IER,  0x00);

    uart_out(UART_LCR, UART_LCR_8N1);
    uart_out(UART_FCR, UART_FCR_ENABLE | UART_FCR_CLR_RX |
                        UART_FCR_CLR_TX | UART_FCR_TRIG_14);
    uart_out(UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

    /* Loopback test */
    uart_out(UART_MCR, 0x1E);
    uart_out(UART_DATA, 0xAE);
    if (uart_in(UART_DATA) != 0xAE) {
        uart_port = 0;
        return;
    }

    uart_out(UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
    uart_ready = true;
}

#elif defined(__aarch64__)

/*
 * ARM PL011 UART driver for QEMU aarch64 virt.
 *
 * PL011 has 32-bit registers at word-aligned offsets, which is
 * completely different from the 16550 byte-offset layout.
 */

#define PL011_DR     0x000   /* Data Register */
#define PL011_FR     0x018   /* Flag Register */
#define PL011_IBRD   0x024   /* Integer Baud Rate Divisor */
#define PL011_FBRD   0x028   /* Fractional Baud Rate Divisor */
#define PL011_LCR_H  0x030   /* Line Control */
#define PL011_CR     0x034   /* Control Register */
#define PL011_IMSC   0x038   /* Interrupt Mask Set/Clear */
#define PL011_ICR    0x044   /* Interrupt Clear */

#define PL011_FR_TXFF  (1u << 5)   /* Transmit FIFO full */
#define PL011_FR_RXFE  (1u << 4)   /* Receive FIFO empty */

#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE    (1u << 8)
#define PL011_CR_RXE    (1u << 9)

#define PL011_LCR_WLEN8 (3u << 5)  /* 8-bit word */
#define PL011_LCR_FEN   (1u << 4)  /* Enable FIFOs */

static volatile uint32_t *pl011_base = NULL;

static inline uint32_t pl011_read(uint32_t off)
{
    return pl011_base ? *(volatile uint32_t *)((uint8_t *)pl011_base + off) : 0;
}

static inline void pl011_write(uint32_t off, uint32_t val)
{
    if (pl011_base) *(volatile uint32_t *)((uint8_t *)pl011_base + off) = val;
}

void serial_init(uint16_t port)
{
    (void)port;
    uart_ready = false;

#ifdef SERIAL_MMIO_BASE
    pl011_base = (volatile uint32_t *)SERIAL_MMIO_BASE;
#else
    return;
#endif

    pl011_write(PL011_CR, 0);
    pl011_write(PL011_ICR, 0x7FF);
    pl011_write(PL011_IMSC, 0);

    /* 115200 baud at 24 MHz reference clock (QEMU virt default):
     * divisor = 24000000 / (16 * 115200) = 13.0208
     * IBRD = 13, FBRD = round(0.0208 * 64) = 1 */
    pl011_write(PL011_IBRD, 13);
    pl011_write(PL011_FBRD, 1);

    pl011_write(PL011_LCR_H, PL011_LCR_WLEN8 | PL011_LCR_FEN);
    pl011_write(PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);

    uart_ready = true;
}

static void pl011_putchar(char c)
{
    while (pl011_read(PL011_FR) & PL011_FR_TXFF)
        ;
    pl011_write(PL011_DR, (uint32_t)(uint8_t)c);
}

static int pl011_getchar(void)
{
    if (pl011_read(PL011_FR) & PL011_FR_RXFE)
        return -1;
    return (int)(pl011_read(PL011_DR) & 0xFF);
}

#else

/* ── Non-x86, non-aarch64: generic MMIO 16550 UART ──── */
static volatile uint8_t *uart_base = NULL;

static inline void uart_out(uint16_t reg, uint8_t val)
{
    if (uart_base) uart_base[reg] = val;
}

static inline uint8_t uart_in(uint16_t reg)
{
    return uart_base ? uart_base[reg] : 0;
}

void serial_init(uint16_t port)
{
    (void)port;

#ifdef SERIAL_MMIO_BASE
    uart_base = (volatile uint8_t *)SERIAL_MMIO_BASE;
#else
    uart_base = NULL;
    return;
#endif

    uart_ready = false;

    uart_out(UART_IER, 0x00);

    uart_out(UART_LCR, UART_LCR_DLAB);
    uart_out(UART_DATA, 0x01);
    uart_out(UART_IER,  0x00);

    uart_out(UART_LCR, UART_LCR_8N1);
    uart_out(UART_FCR, UART_FCR_ENABLE | UART_FCR_CLR_RX |
                        UART_FCR_CLR_TX | UART_FCR_TRIG_14);
    uart_out(UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

    /* Loopback test */
    uart_out(UART_MCR, 0x1E);
    uart_out(UART_DATA, 0xAE);
    uint8_t test = uart_in(UART_DATA);
    if (test != 0xAE) {
        /* Some MMIO UARTs don't support loopback; accept anyway */
        uart_out(UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
        uart_ready = true;
        return;
    }

    uart_out(UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
    uart_ready = true;
}

#endif /* SERIAL_USE_PIO / __aarch64__ / fallback */

/* ── Common API ──────────────────────────────────────── */

void serial_putchar(char c)
{
    if (!uart_ready) return;

#if defined(__aarch64__)
    if (c == '\n')
        pl011_putchar('\r');
    pl011_putchar(c);
#else
    while (!(uart_in(UART_LSR) & UART_LSR_TX_EMPTY))
        ;
    if (c == '\n') {
        uart_out(UART_DATA, '\r');
        while (!(uart_in(UART_LSR) & UART_LSR_TX_EMPTY))
            ;
    }
    uart_out(UART_DATA, (uint8_t)c);
#endif
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putchar(*s++);
}

int serial_read(void)
{
    if (!uart_ready) return -1;

#if defined(__aarch64__)
    return pl011_getchar();
#else
    if (!(uart_in(UART_LSR) & UART_LSR_RX_READY))
        return -1;
    return uart_in(UART_DATA);
#endif
}

void serial_write(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        serial_putchar((char)p[i]);
}
