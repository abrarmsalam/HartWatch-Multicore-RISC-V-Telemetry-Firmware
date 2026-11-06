/*
 * kprintf.c -- a ~70-line printf, because there is no C library on bare metal.
 *
 * Everything the standard library normally gives you (printf, memcpy, malloc, ...)
 * is code that someone has to write. Here we write only what we need.
 */
#include <stdarg.h>
#include <stdint.h>
#include "kprintf.h"
#include "platform.h"
#include "spinlock.h"
#include "uart.h"
#include "csr.h"

static spinlock_t print_lock;

static void put_char(char c)
{
    uart_putc(UART0_BASE, c);
}

static void put_padding(int n, char pad)
{
    while (n-- > 0)
        put_char(pad);
}

static void put_unsigned(unsigned long v, unsigned base, int width, char pad, int negative)
{
    char digits[24];
    int n = 0;

    if (v == 0)
        digits[n++] = '0';
    while (v) {
        digits[n++] = "0123456789abcdef"[v % base];
        v /= base;
    }

    int total = n + (negative ? 1 : 0);
    if (pad == ' ')
        put_padding(width - total, ' ');
    if (negative)
        put_char('-');
    if (pad == '0')
        put_padding(width - total, '0');
    while (n)
        put_char(digits[--n]);
}

static void vformat(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put_char(*fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        int width = 0, longs = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }

        switch (*fmt) {
        case 'd': {
            long v = longs ? va_arg(ap, long) : va_arg(ap, int);
            put_unsigned(v < 0 ? -(unsigned long)v : (unsigned long)v, 10, width, pad, v < 0);
            break;
        }
        case 'u':
            put_unsigned(longs ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int),
                         10, width, pad, 0);
            break;
        case 'x':
            put_unsigned(longs ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int),
                         16, width, pad, 0);
            break;
        case 'c':
            put_char((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            put_padding(width - len, ' ');
            while (*s)
                put_char(*s++);
            break;
        }
        case '%':
            put_char('%');
            break;
        default:               /* unknown conversion: show it verbatim */
            put_char('%');
            put_char(*fmt);
            break;
        }
        if (!*fmt)
            break;
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    spin_lock(&print_lock);
    vformat(fmt, ap);
    spin_unlock(&print_lock);
    va_end(ap);
}

void kpanic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    print_lock.locked = 0;                    /* we are dying: take the lock by force */
    kprintf("\nPANIC on hart %lu: ", csr_read(mhartid));
    spin_lock(&print_lock);
    vformat(fmt, ap);
    spin_unlock(&print_lock);
    va_end(ap);
    kprintf("\n");
    platform_exit();
}
