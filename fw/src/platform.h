/*
 * platform.h -- typed access to memory-mapped hardware registers.
 *
 * "Memory-mapped I/O" means a device's registers appear at fixed addresses, and you
 * talk to the device with ordinary loads and stores. `volatile` tells the compiler
 * "this memory can change behind your back / writes have side effects -- do not
 * cache, reorder away, or delete these accesses."
 */
#ifndef HW_PLATFORM_H
#define HW_PLATFORM_H

#include <stdint.h>
#include "config.h"

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG64(addr) (*(volatile uint64_t *)(uintptr_t)(addr))

/* ---- CLINT (SiFive "core-local interruptor") ---------------------------------
 *   msip[h]      @ +0x0000 + 4*h : write 1 -> raises a software interrupt on hart h
 *   mtimecmp[h]  @ +0x4000 + 8*h : timer interrupt fires when mtime >= mtimecmp[h]
 *   mtime        @ +0xBFF8       : ONE free-running counter shared by every hart
 * That shared mtime is what lets the E51 put all four U54s on a common timeline.  */
#define CLINT_MSIP(h)      REG32(CLINT_BASE + 0x0000UL + 4UL * (h))
#define CLINT_MTIMECMP(h)  REG64(CLINT_BASE + 0x4000UL + 8UL * (h))
#define CLINT_MTIME        REG64(CLINT_BASE + 0xBFF8UL)

/* ---- GPIO (only used to ask QEMU to exit) ---------------------------------- */
#define GPIO_OUTPUT_EN     REG32(GPIO_BASE + 0x08)
#define GPIO_OUTPUT_VAL    REG32(GPIO_BASE + 0x0C)

#endif
