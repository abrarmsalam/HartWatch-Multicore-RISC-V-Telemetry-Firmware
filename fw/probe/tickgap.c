/*
 * tickgap.c -- why do tick gaps widen under -icount when the U54s are busy?
 *
 * A separate image (`make tickgap GAP="..."`), like the counter probe. Each -D below changes
 * one thing, so a difference between two runs has only one cause. It reports the gap between
 * ticks and, more usefully, how late each tick arrived against its deadline.
 */
#include <stdint.h>
#include "csr.h"
#include "hart.h"
#include "kprintf.h"
#include "platform.h"
#include "sample.h"
#include "timer.h"
#include "uart.h"
#include "workloads.h"

#ifndef GAP_BUSY
#define GAP_BUSY    0x1E    /* bit per hart: run a workload (else sleep in wfi) */
#endif
#ifndef GAP_TICKS
#define GAP_TICKS   0x1E    /* bit per hart: take timer ticks */
#endif
#ifndef GAP_WL
#define GAP_WL      4       /* 0 alu, 1 branch, 2 load, 3 store, 4 mixed, 5 mixed rotated by one hart */
#endif
#ifndef GAP_SAMPLE
#define GAP_SAMPLE  1       /* 1: the ISR does the real snapshot and record fill */
#endif
#ifndef GAP_PAD
#define GAP_PAD     0       /* extra instructions per ISR, x4 (nop loop) */
#endif
#ifndef GAP_PERIOD
#define GAP_PERIOD  1000    /* tick period in us */
#endif
#ifndef GAP_E51_TICK
#define GAP_E51_TICK 1      /* 1: E51 keeps its 100 Hz tick; 0: sleeps until poked */
#endif
#define COUNT       2000

#define CHUNK       1000
#define LOAD_BASE   0x84000000UL
#define STORE_BASE  0x86000000UL
#define MASK        0x01FFFFFFUL
#define STRIDE      (4096 + 64)

hart_state_t g_hart[NUM_HARTS];

static struct {
    uint64_t late_min, late_max, wide;  /* lateness in us; ticks more than 1 us off period */
    uint64_t prev;
    sample_t rec;
} __attribute__((aligned(64))) g_stat[NUM_HARTS];

/* Replaces the real sampler: same ISR work, plus lateness bookkeeping. */
void sampler_tick(unsigned hart, uint64_t mtime, uint64_t mepc);
void sampler_tick(unsigned hart, uint64_t mtime, uint64_t mepc)
{
    uint64_t late = mtime - g_hart[hart].next_deadline;

    if (late < g_stat[hart].late_min || g_hart[hart].ticks == 0)
        g_stat[hart].late_min = late;
    if (late > g_stat[hart].late_max)
        g_stat[hart].late_max = late;
    if (g_hart[hart].ticks) {
        uint64_t gap = mtime - g_stat[hart].prev;
        if (gap + 1 < GAP_PERIOD || gap > GAP_PERIOD + 1)
            g_stat[hart].wide++;
    }
    g_stat[hart].prev = mtime;

#if GAP_SAMPLE
    hpm_snapshot_t snap;
    hpm_snapshot(&snap);
    sample_fill(&g_stat[hart].rec, (uint32_t)g_hart[hart].ticks, (uint8_t)hart, &snap, mtime, mepc);
#else
    (void)mepc;
#endif
    for (unsigned i = 0; i < GAP_PAD; i++)
        __asm__ volatile("nop");
}

static void work(uint64_t hart)
{
    unsigned which = GAP_WL;

    if (GAP_WL == 4)
        which = (unsigned)hart - 1;
    if (GAP_WL == 5)
        which = (unsigned)hart % 4;

    switch (which) {
    case 0: wl_alu(CHUNK); break;
    case 1: wl_branch(CHUNK); break;
    case 2: wl_stride(CHUNK, LOAD_BASE, STRIDE, MASK); break;
    default: wl_store(CHUNK, STORE_BASE, STRIDE, MASK); break;
    }
}

void hart0_main(uint64_t hartid)
{
    uart_init(UART0_BASE);
#if GAP_E51_TICK
    timer_start(0, 10000, 0);
#else
    csr_write(mie, MIE_MSIE);       /* wfi returns on a poke; MIE stays 0, so no trap */
#endif
    __asm__ volatile("fence" ::: "memory");
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        CLINT_MSIP(h) = 1;

    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        if (!(GAP_TICKS & (1u << h)))
            continue;
        while (!__atomic_load_n(&g_hart[h].done, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("wfi");
#if !GAP_E51_TICK
            CLINT_MSIP(hartid) = 0;
#endif
        }
    }
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        if (!(GAP_TICKS & (1u << h)))
            continue;
        hart_state_t *s = &g_hart[h];
        kprintf("h%u gap %lu..%lu wide %lu late %lu..%lu | ", h, s->min_gap, s->max_gap,
                g_stat[h].wide, g_stat[h].late_min, g_stat[h].late_max);
    }
    kprintf("\nGAP DONE\n");
    platform_exit();
}

void hart_main(uint64_t hartid)
{
    hart_state_t *me = &g_hart[hartid];

    hpm_init();
    if (GAP_TICKS & (1u << hartid))
        timer_start((unsigned)hartid, GAP_PERIOD, COUNT);

    if (GAP_BUSY & (1u << hartid)) {
        while (!((GAP_TICKS & (1u << hartid)) ? __atomic_load_n(&me->done, __ATOMIC_ACQUIRE) : 0))
            work(hartid);
    } else if (GAP_TICKS & (1u << hartid)) {
        while (!__atomic_load_n(&me->done, __ATOMIC_ACQUIRE))
            __asm__ volatile("wfi");
    }
    CLINT_MSIP(0) = 1;
    for (;;)
        __asm__ volatile("wfi");
}
