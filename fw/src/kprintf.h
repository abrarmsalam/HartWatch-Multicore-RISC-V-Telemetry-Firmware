#ifndef HW_KPRINTF_H
#define HW_KPRINTF_H

/* Tiny printf for bare metal. Supports: %s %c %d %u %x, an optional 0 flag, a width,
 * and the l / ll length modifiers (e.g. %08lx, %5lu). Output goes to UART0.
 * Safe to call from several harts at once (it takes a lock). */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Print a fatal message, then stop the machine. Ignores the print lock so it can
 * never deadlock, even if called from a trap handler. */
void kpanic(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* Ask QEMU to exit (see power.c). */
void platform_exit(void) __attribute__((noreturn));

#endif
