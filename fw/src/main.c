/*
 * main.c -- Day 1 bring-up.
 *
 *   Step 1: hart 0 boots and prints.
 *   Step 2: hart 0 wakes harts 1-4; each reports its own ISA.
 *   Step 3: each U54 takes 2000 timer interrupts at 1 kHz; hart 0 checks the timing.
 */
#include <stdint.h>
#include "csr.h"
#include "hart.h"
#include "kprintf.h"
#include "platform.h"
#include "timer.h"
#include "uart.h"

#define SAMPLE_HZ        1000
#define SAMPLE_PERIOD_US (TIMEBASE_HZ / SAMPLE_HZ)    /* 1000 us */
#define TEST_TICKS       2000                          /* 2 seconds of ticks */

hart_state_t g_hart[NUM_HARTS];

/* Turn the misa CSR into a string like "rv64acdfimsu". Bit i set => extension letter 'a'+i. */
static const char *isa_string(uint64_t misa, char *buf)
{
    int n = 0;
    buf[n++] = 'r';
    buf[n++] = 'v';
    buf[n++] = ((misa >> 62) == 2) ? '6' : '3';     /* MXL field: 2 => 64-bit */
    buf[n++] = ((misa >> 62) == 2) ? '4' : '2';
    for (int i = 0; i < 26; i++)
        if (misa & (1UL << i))
            buf[n++] = 'a' + i;
    buf[n] = 0;
    return buf;
}

/* Sleep (wfi) until *flag becomes 1. Hart 0 has its own 100 Hz tick running, so wfi always wakes. */
static void wait_for_flag(volatile uint32_t *flag, const char *what, unsigned hart)
{
    uint64_t start = CLINT_MTIME;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (CLINT_MTIME - start > 10 * TIMEBASE_HZ)          /* 10 seconds */
            kpanic("hart %u never reported '%s'", hart, what);
        __asm__ volatile("wfi");
    }
}

/* ------------------------------------------------------------------------------
 * Hart 0: the E51 monitor core.
 * ------------------------------------------------------------------------------ */
void hart0_main(uint64_t hartid)
{
    char isa[40];

    uart_init(UART0_BASE);
    kprintf("\n=== HartWatch firmware ===\n");
    g_hart[hartid].misa = csr_read(misa);
    kprintf("hart %lu: monitor core up, isa=%s\n", hartid, isa_string(g_hart[hartid].misa, isa));

    /* A slow 100 Hz tick for ourselves so we can sleep in wfi while waiting, instead of
     * spinning (a spinning hart would steal CPU time from QEMU's other hart threads). */
    timer_start(0, 10000, 0);

    /* ---- Step 2: wake the application cores ------------------------------------ */
    __asm__ volatile("fence" ::: "memory");     /* publish our writes before the poke */
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        CLINT_MSIP(h) = 1;

    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        wait_for_flag(&g_hart[h].online, "online", h);

    kprintf("hart 0: all %d harts are online\n", NUM_HARTS);
    for (unsigned h = 0; h < NUM_HARTS; h++)
        kprintf("  hart %u  isa=%s\n", h, isa_string(g_hart[h].misa, isa));

    /* ---- Step 3: the U54s are now running their timer self-test ------------------ */
    kprintf("hart 0: waiting for each U54 to take %d timer interrupts at %d Hz...\n",
            TEST_TICKS, SAMPLE_HZ);
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        wait_for_flag(&g_hart[h].done, "timer self-test", h);

    kprintf("\n hart  ticks  mean_gap_us  min_gap_us  max_gap_us  late(>2x)\n");
    int ok = 1;
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        hart_state_t *s = &g_hart[h];
        uint64_t mean = (s->last_tick_time - s->first_tick_time) / (s->ticks - 1);
        kprintf(" %4u  %5lu  %11lu  %10lu  %10lu  %9lu\n",
                h, s->ticks, mean, s->min_gap, s->max_gap, s->late_ticks);
        if (s->ticks != TEST_TICKS)
            ok = 0;
        if (mean < SAMPLE_PERIOD_US * 95 / 100 || mean > SAMPLE_PERIOD_US * 105 / 100)
            ok = 0;                              /* mean rate must be within +/-5% */
    }
    kprintf("\n");

    if (!ok)
        kpanic("timer self-test failed: wrong tick count or mean rate off by more than 5%%");
    kprintf("ALL TESTS PASSED\n");
    platform_exit();
}

/* ------------------------------------------------------------------------------
 * Harts 1-4: the U54 application cores. boot.S calls this once the hart is woken.
 * ------------------------------------------------------------------------------ */
void hart_main(uint64_t hartid)
{
    hart_state_t *me = &g_hart[hartid];

    me->misa = csr_read(misa);
    __atomic_store_n(&me->online, 1, __ATOMIC_RELEASE);

    /* Step 3: run a fixed number of 1 kHz timer interrupts, then stop. The interrupt
     * handler (timer_irq) records statistics and sets me->done when finished. */
    timer_start((unsigned)hartid, SAMPLE_PERIOD_US, TEST_TICKS);

    for (;;)
        __asm__ volatile("wfi");
}
