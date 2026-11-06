/*
 * ring_stress.c -- hammer the rings with real RISC-V code (make stress).
 *
 * Four U54s produce as fast as they are allowed and the E51 consumes all four rings. No ISR,
 * no wire: this tests the ring alone. Rings are tiny, so full, empty and wrap-around happen
 * constantly. Every field of a record is computed from seq, so the E51 recomputes the whole
 * record and any torn or stale read shows up.
 *
 * Run it WITHOUT -icount (the make target's default). Under -icount the harts take turns on
 * one host thread, so the two sides of a ring are never truly concurrent.
 *
 * What a pass shows, and does not show: docs/ring.md.
 */
#include <stdint.h>
#include "config.h"
#include "csr.h"
#include "kprintf.h"
#include "platform.h"
#include "ring.h"
#include "uart.h"

/* Run until this many records are VERIFIED. A fixed number of attempts would not do: hitting
 * a full ring is so cheap that producers finish quickly, having delivered few records. */
#ifndef STRESS_TARGET
#define STRESS_TARGET   20000000UL
#endif
#ifndef STRESS_SLOTS
#define STRESS_SLOTS    64
#endif

/* Spin iterations between attempts. The default keeps every ring full, which tests the drop
 * path and the producer waiting on tail. STRESS_PACE slows all producers below what the E51
 * can drain, so the rings stay near empty and the consumer sits right behind the producer:
 * the case that tests publishing a record. */
#ifdef STRESS_PACE
static const unsigned pacing[NUM_HARTS] = { 0, STRESS_PACE, STRESS_PACE, STRESS_PACE, STRESS_PACE };
#else
static const unsigned pacing[NUM_HARTS] = { 0, 0, 0, 100, 400 };
#endif

static struct {
    volatile uint32_t done;
    uint64_t attempts;
} __attribute__((aligned(64))) g_producer[NUM_HARTS];

static volatile uint32_t g_stop __attribute__((aligned(64)));

static ring_t *hart_ring(unsigned hart)
{
    return (ring_t *)(L2LIM_BASE + (uintptr_t)(hart - FIRST_APP_HART) * RING_BYTES(STRESS_SLOTS));
}

static void fill(sample_t *s, uint32_t seq, uint8_t hart)
{
    uint64_t v = seq;
    hpm_snapshot_t snap = {
        .mcycle = v * 3, .minstret = v * 5, .hpm = { v * 7, v * 11, v * 13 },
    };
    sample_fill(s, seq, hart, &snap, v * 17, v * 19);
}

void trap_handler(uint64_t mcause, uint64_t mepc, uint64_t mtval)
{
    kpanic("unexpected trap, mcause=0x%lx, pc=0x%lx, mtval=0x%lx", mcause, mepc, mtval);
}

/* Every word of the record, compared with what seq says it must be. */
static int record_is_wrong(const sample_t *s, uint8_t hart)
{
    sample_t want;
    const uint64_t *a = (const uint64_t *)s, *b = (const uint64_t *)&want;

    fill(&want, s->seq, hart);
    for (unsigned i = 0; i < sizeof(sample_t) / 8; i++)
        if (a[i] != b[i])
            return 1;
    return 0;
}

void hart_main(uint64_t hartid)
{
    ring_t *ring = hart_ring((unsigned)hartid);

    uint64_t i;

    for (i = 0; !__atomic_load_n(&g_stop, __ATOMIC_RELAXED); i++) {
        sample_t *s = ring_reserve(ring);

        if (s) {
            fill(s, (uint32_t)i, (uint8_t)hartid);
            ring_commit(ring);
        }
        for (unsigned d = 0; d < pacing[hartid]; d++)
            __asm__ volatile("nop");
    }
    g_producer[hartid].attempts = i;
    __atomic_store_n(&g_producer[hartid].done, 1, __ATOMIC_RELEASE);
    for (;;)
        __asm__ volatile("wfi");
}

void hart0_main(uint64_t hartid)
{
    static struct {
        uint64_t received, bad, backwards, gaps, max_occupancy, near_empty, polls;
        uint64_t next;      /* the attempt after the newest one seen, not limited to 32 bits */
    } st[NUM_HARTS];
    unsigned running = NUM_HARTS - FIRST_APP_HART;
    uint64_t verified = 0;

    (void)hartid;
    uart_init(UART0_BASE);
    kprintf("\n=== HartWatch ring stress: target %lu verified records, %d slots ===\n",
            STRESS_TARGET, STRESS_SLOTS);

    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        ring_init(hart_ring(h), STRESS_SLOTS);
    __asm__ volatile("fence" ::: "memory");
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++)
        CLINT_MSIP(h) = 1;

    while (running) {
        running = 0;
        for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
            ring_t *ring = hart_ring(h);
            /* Read done first: if it is set, everything the producer wrote is visible, so an
             * empty ring afterwards really means this hart has finished. */
            uint32_t done = __atomic_load_n(&g_producer[h].done, __ATOMIC_ACQUIRE);
            uint64_t ready = ring_occupancy(ring);

            if (ready > st[h].max_occupancy)
                st[h].max_occupancy = ready;
            if (ready <= 1)
                st[h].near_empty++;
            st[h].polls++;
            if (!done || ready)
                running++;
            for (unsigned n = 0; n < 8 && n < ready; n++) {
                const sample_t *s = ring_peek(ring);

                if (record_is_wrong(s, (uint8_t)h))
                    st[h].bad++;
                /* seq is 32 bits and fast harts wrap it. The unsigned difference is the number
                 * of attempts skipped, even across a wrap. Half the range or more means the
                 * record is older than the last one. */
                uint32_t skipped = s->seq - (uint32_t)st[h].next;
                if (skipped >= 0x80000000u) {
                    st[h].backwards++;
                } else {
                    st[h].gaps += skipped;
                    st[h].next += (uint64_t)skipped + 1;
                }
                st[h].received++;
                verified++;
                ring_consume(ring);
            }
        }
        if (verified >= STRESS_TARGET)
            __atomic_store_n(&g_stop, 1, __ATOMIC_RELAXED);
    }

    kprintf(" hart  pacing   attempts   received    dropped   seq_gaps   bad  backwards  max_occ       laps  near_empty%%\n");
    uint64_t total = 0, problems = 0;
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        uint64_t dropped = ring_dropped(hart_ring(h));
        uint64_t attempts = g_producer[h].attempts;
        uint64_t gaps = st[h].gaps + (attempts - st[h].next);

        kprintf(" %4u  %6u %10lu %10lu %10lu %10lu %5lu %10lu %8lu %10lu %11lu\n", h, pacing[h],
                attempts, st[h].received, dropped, gaps, st[h].bad, st[h].backwards,
                st[h].max_occupancy, st[h].received / STRESS_SLOTS,
                st[h].near_empty * 100 / st[h].polls);
        total += st[h].received;
        problems += st[h].bad + st[h].backwards;
        if (st[h].received + dropped != attempts || gaps != dropped)
            problems++;
    }
    kprintf("records verified: %lu, problems: %lu\n", total, problems);
    if (problems)
        kpanic("ring stress failed");
    kprintf("STRESS PASSED\n");
    platform_exit();
}
