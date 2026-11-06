/*
 * uart.c -- driver for the SiFive UART (the serial port on the FU540).
 *
 * Register map (offsets from the UART base address):
 *   0x00 txdata : write a byte to send it. Bit 31 reads 1 when the transmit FIFO is full.
 *   0x04 rxdata : received byte
 *   0x08 txctrl : bit 0 = transmitter enable
 *   0x0C rxctrl : bit 0 = receiver enable
 *   0x18 div    : baud-rate divisor
 *
 * On real silicon you must also program `div` from the core clock to get e.g. 115200
 * baud. QEMU ignores the divisor and transmits instantly, so we skip it for now --
 * this is one of the "QEMU vs. silicon" differences to note in the porting table.
 */
#include "uart.h"
#include "platform.h"

#define UART_TXDATA  0x00
#define UART_RXCTRL  0x0C
#define UART_TXCTRL  0x08
#define UART_TX_FULL 0x80000000u

void uart_init(uintptr_t base)
{
    REG32(base + UART_TXCTRL) = 1;   /* transmitter on */
    REG32(base + UART_RXCTRL) = 1;   /* receiver on    */
}

void uart_putc(uintptr_t base, char c)
{
    while (REG32(base + UART_TXDATA) & UART_TX_FULL) {
        /* spin until there is room in the FIFO */
    }
    REG32(base + UART_TXDATA) = (uint8_t)c;
}
