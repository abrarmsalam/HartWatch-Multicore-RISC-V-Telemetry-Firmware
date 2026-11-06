/*
 * power.c -- how the firmware tells QEMU "I am finished".
 *
 * Real hardware never "finishes", but an automated test must be able to stop. On the
 * sifive_u machine, GPIO pin 10 is wired to the board's reset line (Linux uses the
 * same trick to reboot it; see `gpio-restart` in the device tree). We run QEMU with
 * -no-reboot, which turns "reset requested" into "exit QEMU".
 */
#include "kprintf.h"
#include "platform.h"

void platform_exit(void)
{
    GPIO_OUTPUT_EN  = 1u << 10;
    GPIO_OUTPUT_VAL = 1u << 10;
    for (;;)
        __asm__ volatile("wfi");
}
