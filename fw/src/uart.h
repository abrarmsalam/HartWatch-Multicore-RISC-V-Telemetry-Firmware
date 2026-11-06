#ifndef HW_UART_H
#define HW_UART_H

#include <stdint.h>

void uart_init(uintptr_t base);
void uart_putc(uintptr_t base, char c);

#endif
