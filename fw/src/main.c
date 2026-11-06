/*
 * main.c -- bring-up and sampling self-test.
 *
 *   Step 1: hart 0 boots and prints.
 *   Step 2: hart 0 wakes harts 1-4; each reports its own ISA.
 *   Step 3: each U54 runs a workload through 2000 timer interrupts at 1 kHz, sampling on
 *           every tick. Hart 0 drains the rings to UART1 as they fill, and validates every
 *           record on its way through. The host decoder then checks the stream independently.
 */
#include <stdint.h>
#include "csr.h"
#include "drain.h"
#include "hart.h"
#include "kprintf.h"
#include "platform.h"
#include "sampler.h"
#include "timer.h"
#include "uart.h"
#include "workloads.h"

#ifndef SAMPLE_HZ
#define SAMPLE_HZ        1000
#endif
#ifndef TEST_TICKS
#define TEST_TICKS       2000
#endif
#ifndef EXPECT_DROPS
#define EXPECT_DROPS     0      /* 1: this build is meant to overflow its rings */
#endif
#define SAMPLE_PERIOD_US (TIMEBASE_HZ / SAMPLE_HZ)

#define WL_CHUNK_ITERS   1000           /* short calls, so a hart notices `done` promptly */
#define WL_LOAD_BASE     0x84000000UL   /* free DRAM above the firmware image */
#define WL_STORE_BASE    0x86000000UL
#define WL_ARRAY_MASK    0x01FFFFFFUL   /* 32 MiB per array */
#define WL_STRIDE        (4096 + 64)    /* a new page on every access */

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

/* Sleep (wfi) until *flag becomes 1. Hart 0 has its own drain tick running, so wfi always wakes. */
static void wait_for_flag(volatile uint32_t *flag, const char *what, unsigned hart)
{
    uint64_t start = CLINT_MTIME;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (CLINT_MTIME - start > 10 * TIMEBASE_HZ)          /* 10 seconds */
            kpanic("hart %u never reported '%s'", hart, what);
        __asm__ volatile("wfi");
    }
}

/* One workload per U54, so the self-test can tell the harts' samples apart. */
static void run_alu(void)    { wl_alu(WL_CHUNK_ITERS); }
static void run_branch(void) { wl_branch(WL_CHUNK_ITERS); }
static void run_load(void)   { wl_stride(WL_CHUNK_ITERS, WL_LOAD_BASE, WL_STRIDE, WL_ARRAY_MASK); }
static void run_store(void)  { wl_store(WL_CHUNK_ITERS, WL_STORE_BASE, WL_STRIDE, WL_ARRAY_MASK); }

static const struct {
    const char *name;
    void (*run)(void);
    void (*start)(void);
    void (*end)(void);
} hart_workload[NUM_HARTS] = {
    [1] = { "alu",    run_alu,    (void (*)(void))wl_alu,    wl_alu_end },
    [2] = { "branch", run_branch, (void (*)(void))wl_branch, wl_branch_end },
    [3] = { "load",   run_load,   (void (*)(void))wl_stride, wl_stride_end },
    [4] = { "store",  run_store,  (void (*)(void))wl_store,  wl_store_end },
};

/* Each U54's ring in L2-LIM. L2-LIM is not cleared at boot, so hart 0 initialises them. */
static ring_t *hart_ring(unsigned hart)
{
    return (ring_t *)(L2LIM_BASE + (uintptr_t)(hart - FIRST_APP_HART) * RING_BYTES(RING_SLOTS));
}

/* What hart 0 learns about each U54's records as they pass through the drain. */
static struct {
    unsigned count, bad, in_workload;
    uint64_t seq_gaps;
    sample_t first, prev;
} g_seen[NUM_HARTS];

static void check_sample(unsigned hart, const sample_t *s)
{
    uintptr_t start = (uintptr_t)hart_workload[hart].start;
    uintptr_t end = (uintptr_t)hart_workload[hart].end;
    unsigned err = sample_check_next(g_seen[hart].count ? &g_seen[hart].prev : 0, s, (uint8_t)hart);

    if (g_seen[hart].count)
        g_seen[hart].seq_gaps += s->seq - g_seen[hart].prev.seq - 1;
    if (EXPECT_DROPS)
        err &= ~SAMPLE_ERR_SEQ;             /* a gap is correct here; the totals are checked below */
    if (err)
        g_seen[hart].bad++;
    if (s->mepc >= start && s->mepc < end)
        g_seen[hart].in_workload++;
    g_seen[hart].prev = *s;
    if (g_seen[hart].count++ == 0)
        g_seen[hart].first = *s;
}

/* Print one hart's totals and return the number of problems found. */
static unsigned report_samples(unsigned hart)
{
    const sample_t *first = &g_seen[hart].first, *last = &g_seen[hart].prev;
    uint64_t dropped = ring_dropped(hart_ring(hart));
    unsigned count = g_seen[hart].count, bad = g_seen[hart].bad;

    kprintf(" %4u %8s %7u %5lu %4u %10u %9lu %10lu %5lu\n", hart, hart_workload[hart].name,
            count, dropped, bad, g_seen[hart].in_workload, last->hpm[0] - first->hpm[0],
            last->hpm[1] - first->hpm[1], last->hpm[2] - first->hpm[2]);

    if (count + dropped != TEST_TICKS || sampler_attempts(hart) != TEST_TICKS)
        bad++;
    if (EXPECT_DROPS ? dropped == 0 : dropped != 0)
        bad++;
    if (g_seen[hart].in_workload < count * 9 / 10)  /* nearly all time is spent inside the loop */
        bad++;
    return bad;
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

    /* A slow tick for ourselves so we can sleep in wfi between drains, instead of spinning
     * (a spinning hart would steal CPU time from QEMU's other hart threads, and under
     * -icount every instruction it runs moves virtual time forward). */
    timer_start(0, DRAIN_PERIOD_US, 0);

    /* ---- Step 2: wake the application cores ------------------------------------ */
    ring_t *rings[NUM_HARTS] = { 0 };
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        rings[h] = hart_ring(h);
        ring_init(rings[h], RING_SLOTS);
    }
    drain_init(rings, check_sample);
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
    uint64_t start = CLINT_MTIME;
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        while (!__atomic_load_n(&g_hart[h].done, __ATOMIC_ACQUIRE)) {
            if (CLINT_MTIME - start > 60UL * TIMEBASE_HZ)
                kpanic("hart %u never finished its ticks", h);
            drain_poll(0);
            __asm__ volatile("wfi");
        }
    }

    /* Every U54 has stopped, so the rings hold the last records and nothing adds more. */
    drain_poll(1);
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        drain_finish(h, sampler_attempts(h), g_seen[h].bad);

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

    kprintf(" counter backend: %s\n", hpm_backend_name());
    kprintf(" hart workload samples drops  bad mepc_in_wl dtlb_read dtlb_write  itlb\n");
    unsigned bad = 0;
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        bad += report_samples(h);
    kprintf(" wire: %lu frames, %lu bytes on UART1\n\n", drain_frames(), drain_bytes());

    if (bad)
        kpanic("sample self-test failed: %u problem(s)", bad);

    /* The events must follow the workload. Compare each hart's read and write misses with
     * each other: against another hart, swapped selectors would still look plausible. */
    const sample_t *load_first = &g_seen[3].first, *load_last = &g_seen[3].prev;
    const sample_t *store_first = &g_seen[4].first, *store_last = &g_seen[4].prev;
    if (load_last->hpm[0] - load_first->hpm[0] <= 10 * (load_last->hpm[1] - load_first->hpm[1]) ||
        store_last->hpm[1] - store_first->hpm[1] <= 10 * (store_last->hpm[0] - store_first->hpm[0]))
        kpanic("sample self-test failed: DTLB events do not follow the workloads");
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
    hpm_init();
    sampler_init((unsigned)hartid, hart_ring((unsigned)hartid));
    __atomic_store_n(&me->online, 1, __ATOMIC_RELEASE);

    /* Step 3: run a fixed number of 1 kHz timer interrupts, then stop. The interrupt
     * handler (timer_irq) takes a sample, records statistics and sets me->done when finished.
     * A workload runs meanwhile so the samples have something real to report. */
    timer_start((unsigned)hartid, SAMPLE_PERIOD_US, TEST_TICKS);

    while (!__atomic_load_n(&me->done, __ATOMIC_ACQUIRE))
        hart_workload[hartid].run();

    for (;;)
        __asm__ volatile("wfi");
}
