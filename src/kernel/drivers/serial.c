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

#else

/* ── Non-x86: MMIO UART ─────────────────────────────── */
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

#endif /* SERIAL_USE_PIO */

/* ── Common API (shared between PIO and MMIO) ────────── */

void serial_putchar(char c)
{
    if (!uart_ready) return;
    while (!(uart_in(UART_LSR) & UART_LSR_TX_EMPTY))
        ;
    if (c == '\n') {
        uart_out(UART_DATA, '\r');
        while (!(uart_in(UART_LSR) & UART_LSR_TX_EMPTY))
            ;
    }
    uart_out(UART_DATA, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putchar(*s++);
}

int serial_read(void)
{
    if (!uart_ready) return -1;
    if (!(uart_in(UART_LSR) & UART_LSR_RX_READY))
        return -1;
    return uart_in(UART_DATA);
}

void serial_write(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        serial_putchar((char)p[i]);
}
