/*
 * main.c -- bring-up and sampling self-test.
 *
 *   Step 1: hart 0 boots and prints.
 *   Step 2: hart 0 wakes harts 1-4; each reports its own ISA.
 *   Step 3: each U54 runs a workload through 2000 timer interrupts at 1 kHz, sampling on
 *           every tick; hart 0 checks the timing and validates every record.
 */
#include <stdint.h>
#include "csr.h"
#include "hart.h"
#include "kprintf.h"
#include "platform.h"
#include "sampler.h"
#include "timer.h"
#include "uart.h"
#include "workloads.h"

#define SAMPLE_HZ        1000
#define SAMPLE_PERIOD_US (TIMEBASE_HZ / SAMPLE_HZ)    /* 1000 us */
#define TEST_TICKS       2000                          /* 2 seconds of ticks */

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

static sample_t *hart_slots(unsigned hart)
{
    return (sample_t *)(L2LIM_BASE + (uintptr_t)(hart - FIRST_APP_HART) * SAMPLE_SLOTS * sizeof(sample_t));
}

typedef struct {
    uint64_t dtlb_read, dtlb_write, itlb;
} miss_counts_t;

/* Validate every record one hart wrote. Returns the number of problems found. */
static unsigned check_samples(unsigned hart, miss_counts_t *misses)
{
    const sample_log_t *log = sampler_log(hart);
    const sample_t *slots = hart_slots(hart);
    const sample_t *prev = 0;
    uintptr_t start = (uintptr_t)hart_workload[hart].start;
    uintptr_t end = (uintptr_t)hart_workload[hart].end;
    unsigned bad = 0, in_workload = 0;

    for (uint32_t i = 0; i < log->count; i++) {
        if (sample_check_next(prev, &slots[i], (uint8_t)hart))
            bad++;
        if (slots[i].mepc >= start && slots[i].mepc < end)
            in_workload++;
        prev = &slots[i];
    }

    const sample_t *first = &slots[0], *last = &slots[log->count - 1];
    misses->dtlb_read = last->hpm[0] - first->hpm[0];
    misses->dtlb_write = last->hpm[1] - first->hpm[1];
    misses->itlb = last->hpm[2] - first->hpm[2];

    kprintf(" %4u %8s %7u %5lu %4u %10u %9lu %10lu %5lu\n", hart, hart_workload[hart].name,
            log->count, log->dropped, bad, in_workload,
            misses->dtlb_read, misses->dtlb_write, misses->itlb);

    if (log->count != TEST_TICKS || log->dropped != 0)
        bad++;
    if (in_workload < log->count * 9 / 10)  /* nearly all time is spent inside the loop */
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

    kprintf(" counter backend: %s\n", hpm_backend_name());
    kprintf(" hart workload samples drops  bad mepc_in_wl dtlb_read dtlb_write  itlb\n");
    miss_counts_t misses[NUM_HARTS];
    unsigned bad = 0;
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        bad += check_samples(h, &misses[h]);
    kprintf("\n");

    if (bad)
        kpanic("sample self-test failed: %u problem(s)", bad);
    /* The events must follow the workload. Compare each hart's read and write misses with
     * each other: against another hart, swapped selectors would still look plausible. */
    if (misses[3].dtlb_read <= 10 * misses[3].dtlb_write ||
        misses[4].dtlb_write <= 10 * misses[4].dtlb_read)
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
    sampler_init((unsigned)hartid, hart_slots((unsigned)hartid), SAMPLE_SLOTS);
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
