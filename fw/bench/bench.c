/*
 * bench.c -- benchmark firmware (make bench, make gate). See docs/benchmarks.md.
 *
 * Under -icount minstret is one global clock, so it only means "this hart's instructions"
 * when every other hart is asleep. Each mode below is built around that fact:
 *
 *   BENCH_MODE 0  gate: hart 1 alone, no timer, no sampling, each workload once.
 *   BENCH_MODE 1  overhead A: hart 1 runs fixed work, ticking at BENCH_HZ (0 = no timer), with
 *                 the E51 asleep and a ring large enough not to drop. Then hart 1 sleeps and the
 *                 E51 drains alone, which measures drain cost per sample on real records.
 *   BENCH_MODE 2  overhead B: all four U54s tick and work while the E51 drains live. For ring
 *                 occupancy, drops and achieved tick rate. Its instruction counts are global.
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
#include "wire.h"
#include "workloads.h"

#ifndef BENCH_MODE
#define BENCH_MODE      0
#endif
#ifndef BENCH_WL
#define BENCH_WL        0       /* index into workloads[] for mode 1 */
#endif
#ifndef BENCH_ONLY
#define BENCH_ONLY      -1      /* mode 0: run just this workload; -1 = all */
#endif
#ifndef BENCH_HZ
#define BENCH_HZ        0
#endif
#ifndef BENCH_SAMPLE
#define BENCH_SAMPLE    1       /* 0: the timer ticks but the ISR takes no sample */
#endif
#ifndef BENCH_SCALE
#define BENCH_SCALE     1       /* multiplies the iteration counts below */
#endif
#ifndef BENCH_RING_SLOTS
#define BENCH_RING_SLOTS 65536
#endif
#ifndef BENCH_SMALL_STRIDE
#define BENCH_SMALL_STRIDE 64
#endif

#define LOAD_BASE   0x84000000UL
#define STORE_BASE  0x86000000UL
#define ARRAY_MASK  0x01FFFFFFUL
#define LARGE_STRIDE (4096 + 64)

hart_state_t g_hart[NUM_HARTS];

static void run_alu(void)      { wl_alu(5000000UL * BENCH_SCALE); }
static void run_branch(void)   { wl_branch(2000000UL * BENCH_SCALE); }
static void run_load64(void)   { wl_stride(5000000UL * BENCH_SCALE, LOAD_BASE, BENCH_SMALL_STRIDE, ARRAY_MASK); }
static void run_load4k(void)   { wl_stride(5000000UL * BENCH_SCALE, LOAD_BASE, LARGE_STRIDE, ARRAY_MASK); }
static void run_store64(void)  { wl_store(5000000UL * BENCH_SCALE, STORE_BASE, BENCH_SMALL_STRIDE, ARRAY_MASK); }
static void run_store4k(void)  { wl_store(5000000UL * BENCH_SCALE, STORE_BASE, LARGE_STRIDE, ARRAY_MASK); }

static const struct {
    const char *name;
    void (*run)(void);
} workloads[] = {
    { "arith", run_alu },   { "branch", run_branch },   { "load64", run_load64 },
    { "load4k", run_load4k }, { "store64", run_store64 }, { "store4k", run_store4k },
};

#define NUM_WORKLOADS (sizeof(workloads) / sizeof(workloads[0]))

#if BENCH_MODE != 0
static ring_t *hart_ring(unsigned hart)
{
    return (ring_t *)(L2LIM_BASE + (uintptr_t)(hart - FIRST_APP_HART) * RING_BYTES(BENCH_RING_SLOTS));
}
#endif

#if !BENCH_SAMPLE
/* Replaces sampler.c, so the ISR pays for the trap and the timer but takes no sample. */
void sampler_tick(unsigned hart, uint64_t mtime, uint64_t mepc)
{
    (void)hart;
    (void)mtime;
    (void)mepc;
}
#endif

typedef struct {
    uint64_t minstret, mcycle, mtime, dtlb_read, dtlb_write, itlb;
} counters_t;

static void read_counters(counters_t *c)
{
    c->mtime = CLINT_MTIME;
    c->mcycle = csr_read(mcycle);
    c->dtlb_read = csr_read(mhpmcounter3);
    c->dtlb_write = csr_read(mhpmcounter4);
    c->itlb = csr_read(mhpmcounter5);
    c->minstret = csr_read(minstret);
}

static void print_delta(const char *name, const counters_t *a, const counters_t *b)
{
    kprintf("BENCH wl=%s minstret=%lu mcycle=%lu mtime_us=%lu dtlb_read=%lu dtlb_write=%lu itlb=%lu\n",
            name, b->minstret - a->minstret, b->mcycle - a->mcycle, b->mtime - a->mtime,
            b->dtlb_read - a->dtlb_read, b->dtlb_write - a->dtlb_write, b->itlb - a->itlb);
}

#if BENCH_MODE == 0
static void gate_main(void)
{
    hpm_init();
    for (unsigned w = 0; w < NUM_WORKLOADS; w++) {
        counters_t a, b;

        if (BENCH_ONLY >= 0 && w != (unsigned)BENCH_ONLY)
            continue;
        read_counters(&a);
        workloads[w].run();
        read_counters(&b);
        print_delta(workloads[w].name, &a, &b);
    }
    kprintf("BENCH DONE\n");
    platform_exit();
}
#endif

#if BENCH_MODE == 1
static volatile uint32_t g_hart1_done;

static void report_ticks(unsigned hart)
{
    hart_state_t *s = &g_hart[hart];
    uint64_t span = s->last_tick_time - s->first_tick_time;

    kprintf("TICKS hart=%u ticks=%lu span_us=%lu min_gap=%lu max_gap=%lu dropped=%lu\n", hart,
            (uint64_t)s->ticks, s->ticks > 1 ? span : 0, s->ticks > 1 ? s->min_gap : 0,
            s->max_gap, BENCH_HZ && BENCH_SAMPLE ? ring_dropped(hart_ring(hart)) : 0);
}

static void overhead_a_hart1(void)
{
    counters_t a, b;

    hpm_init();
    if (BENCH_HZ)
        timer_start(1, TIMEBASE_HZ / (BENCH_HZ ? BENCH_HZ : 1), 0);
    read_counters(&a);
    workloads[BENCH_WL].run();
    read_counters(&b);
    if (BENCH_HZ)
        timer_stop(1);
    print_delta(workloads[BENCH_WL].name, &a, &b);
    report_ticks(1);
    __atomic_store_n(&g_hart1_done, 1, __ATOMIC_RELEASE);
    CLINT_MSIP(0) = 1;
    for (;;)
        __asm__ volatile("wfi");
}

/* Every other hart is asleep here, so minstret deltas are the E51's own instructions. */
static void measure_drain(void)
{
    static uint8_t data[1024];
    static uint8_t out[1024 + 1024 / 254 + 8];
    uint64_t c0, c1, sink;

    c0 = csr_read(minstret);
    drain_poll(1);
    c1 = csr_read(minstret);
    kprintf("DRAIN samples=%lu frames=%lu bytes=%lu minstret=%lu\n", drain_samples(1), drain_frames(),
            drain_bytes(), c1 - c0);

    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 37 + 1);
    c0 = csr_read(minstret);
    sink = wire_crc32(data, sizeof(data));
    c1 = csr_read(minstret);
    kprintf("CRC bytes=%u minstret=%lu value=0x%lx\n", (unsigned)sizeof(data), c1 - c0, sink);
    c0 = csr_read(minstret);
    sink = wire_cobs_encode(data, sizeof(data), out);
    c1 = csr_read(minstret);
    kprintf("COBS bytes=%u minstret=%lu out=%lu\n", (unsigned)sizeof(data), c1 - c0, sink);
}
#endif

#if BENCH_MODE == 2
#ifndef BENCH_TICKS_LIMIT
#define BENCH_TICKS_LIMIT 0
#endif
static uint64_t g_max_occupancy[NUM_HARTS];
static uint64_t g_start_mtime[NUM_HARTS], g_end_mtime[NUM_HARTS];

static void overhead_b_hart(unsigned hart)
{
    static const unsigned mix[NUM_HARTS] = { 0, 0, 1, 3, 5 };

    hpm_init();
    g_start_mtime[hart] = CLINT_MTIME;
    timer_start(hart, TIMEBASE_HZ / BENCH_HZ, 0);
    workloads[mix[hart]].run();
    timer_stop(hart);
    g_end_mtime[hart] = CLINT_MTIME;
    __atomic_store_n(&g_hart[hart].done, 1, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile("wfi");
}

static void note_occupancy(void)
{
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        uint64_t n = ring_occupancy(hart_ring(h));
        if (n > g_max_occupancy[h])
            g_max_occupancy[h] = n;
    }
}
#endif

void hart_main(uint64_t hartid)
{
#if BENCH_MODE == 0
    (void)hartid;
    gate_main();
#elif BENCH_MODE == 1
    (void)hartid;
    overhead_a_hart1();
#else
    overhead_b_hart((unsigned)hartid);
#endif
}

void hart0_main(uint64_t hartid)
{
    uart_init(UART0_BASE);
    kprintf("\n=== HartWatch bench: mode %d, %d Hz, sample %d, checksum %d ===\n",
            BENCH_MODE, BENCH_HZ, BENCH_SAMPLE, SAMPLE_CHECKSUM);

#if BENCH_MODE == 0
    (void)hartid;
    CLINT_MSIP(1) = 1;
    for (;;)
        __asm__ volatile("wfi");
#elif BENCH_MODE == 1
    ring_t *rings[NUM_HARTS] = { 0 };

    rings[1] = hart_ring(1);
    ring_init(rings[1], BENCH_RING_SLOTS);
    for (unsigned h = 2; h < NUM_HARTS; h++) {  /* drain_poll visits every hart */
        rings[h] = hart_ring(h);
        ring_init(rings[h], 1);
    }
    drain_init(rings, 0);
#if BENCH_SAMPLE
    sampler_init(1, rings[1]);
#endif
    csr_write(mie, MIE_MSIE);
    __asm__ volatile("fence" ::: "memory");
    CLINT_MSIP(1) = 1;
    while (!__atomic_load_n(&g_hart1_done, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("wfi");
        CLINT_MSIP(hartid) = 0;
    }
    measure_drain();
#if BENCH_SAMPLE
    drain_finish(1, sampler_attempts(1), 0);
#endif
    kprintf("BENCH DONE\n");
    platform_exit();
#else
    ring_t *rings[NUM_HARTS] = { 0 };

    (void)hartid;
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        rings[h] = hart_ring(h);
        ring_init(rings[h], BENCH_RING_SLOTS);
        sampler_init(h, rings[h]);
    }
    drain_init(rings, 0);
    timer_start(0, DRAIN_PERIOD_US, 0);
    __asm__ volatile("fence" ::: "memory");
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        CLINT_MSIP(h) = 1;

    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        while (!__atomic_load_n(&g_hart[h].done, __ATOMIC_ACQUIRE)) {
            note_occupancy();
            drain_poll(0);
            __asm__ volatile("wfi");
        }
    }
    note_occupancy();
    drain_poll(1);
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        hart_state_t *s = &g_hart[h];
        kprintf("HART hart=%u ticks=%lu span_us=%lu runtime_us=%lu min_gap=%lu max_gap=%lu attempts=%u "
                "dropped=%lu max_occ=%lu\n", h, (uint64_t)s->ticks, s->last_tick_time - s->first_tick_time,
                g_end_mtime[h] - g_start_mtime[h], s->min_gap, s->max_gap, sampler_attempts(h),
                ring_dropped(rings[h]), g_max_occupancy[h]);
        drain_finish(h, sampler_attempts(h), 0);
    }
    kprintf("WIRE frames=%lu bytes=%lu\n", drain_frames(), drain_bytes());
    kprintf("BENCH DONE\n");
    platform_exit();
#endif
}
