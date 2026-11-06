/*
 * config.h -- build-time constants shared by C code AND assembly (boot.S).
 *
 * Because boot.S includes this file, it may contain ONLY plain #defines
 * (no C types, no prototypes).
 */
#ifndef HW_CONFIG_H
#define HW_CONFIG_H

/* QEMU `-M sifive_u -smp 5`: hart 0 = E51 monitor core, harts 1..4 = U54 app cores. */
#define NUM_HARTS        5
#define MONITOR_HART     0
#define FIRST_APP_HART   1

/* Every hart gets its own stack. 16 KiB is plenty for Day 1. */
#define STACK_SIZE       16384

/* Memory-mapped I/O base addresses. These come straight from the device tree
 * that QEMU generates (see `make dtb`), so they are facts, not guesses. */
#define CLINT_BASE       0x02000000   /* core-local interruptor: timer + software IRQ */
#define L2LIM_BASE       0x08000000   /* 32 MiB of fast on-chip RAM (sample storage) */
#define UART0_BASE       0x10010000   /* console */
#define UART1_BASE       0x10011000   /* telemetry stream (Day 3) */
#define GPIO_BASE        0x10060000   /* pin 10 = "reboot"; we abuse it to exit QEMU */

/* DTB says timebase-frequency = 0xf4240 = 1,000,000 Hz: mtime ticks once per microsecond. */
#define TIMEBASE_HZ      1000000

/* Each sample carries three event counters: DTLB read miss, DTLB write miss, ITLB miss. */
#define HPM_SLOTS        3

/* Test instrumentation, not part of the telemetry design: 1 makes the ISR checksum every
 * record so tests can catch corruption; 0 leaves `check` at 0 and adds no ISR work. Tests use
 * 1. The overhead sweep must use 0, or it measures the test harness. */
#ifndef SAMPLE_CHECKSUM
#define SAMPLE_CHECKSUM  1
#endif

/* One ring per U54 in L2-LIM. Must be a power of two. 4096 slots x 64 bytes = 256 KiB per hart. */
#ifndef RING_SLOTS
#define RING_SLOTS       4096
#endif

#endif
