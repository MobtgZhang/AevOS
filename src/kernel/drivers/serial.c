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

static uint16_t uart_port = 0;

void serial_init(uint16_t port)
{
    uart_port = port;

    outb(port + UART_IER, 0x00);

    /* Set DLAB to configure baud rate divisor */
    outb(port + UART_LCR, UART_LCR_DLAB);
    /* Baud 115200 → divisor = 1 (115200 / 115200) */
    outb(port + UART_DATA, 0x01);  /* divisor low byte */
    outb(port + UART_IER,  0x00);  /* divisor high byte */

    /* 8 bits, no parity, 1 stop bit */
    outb(port + UART_LCR, UART_LCR_8N1);

    /* Enable and clear FIFOs, 14-byte trigger */
    outb(port + UART_FCR, UART_FCR_ENABLE | UART_FCR_CLR_RX |
                           UART_FCR_CLR_TX | UART_FCR_TRIG_14);

    /* DTR + RTS + OUT2 (needed for interrupts) */
    outb(port + UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);

    /* Loopback test */
    outb(port + UART_MCR, 0x1E);
    outb(port + UART_DATA, 0xAE);
    if (inb(port + UART_DATA) != 0xAE) {
        uart_port = 0;
        return;
    }

    /* Restore normal operation */
    outb(port + UART_MCR, UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
}

void serial_putchar(char c)
{
    if (!uart_port) return;
    /* Wait for TX buffer empty */
    while (!(inb(uart_port + UART_LSR) & UART_LSR_TX_EMPTY))
        ;
    if (c == '\n') {
        outb(uart_port + UART_DATA, '\r');
        while (!(inb(uart_port + UART_LSR) & UART_LSR_TX_EMPTY))
            ;
    }
    outb(uart_port + UART_DATA, c);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putchar(*s++);
}

int serial_read(void)
{
    if (!uart_port) return -1;
    if (!(inb(uart_port + UART_LSR) & UART_LSR_RX_READY))
        return -1;
    return inb(uart_port + UART_DATA);
}

void serial_write(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
        serial_putchar((char)p[i]);
}
