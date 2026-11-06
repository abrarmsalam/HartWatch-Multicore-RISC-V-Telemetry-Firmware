/*
 * test_sample.c -- host unit tests for sample.c and sampler.c (make host-test).
 * Built with AddressSanitizer and UBSan, so an overrun fails the run. This is the one
 * place libc is allowed: none of it reaches the firmware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sampler.h"

/* Computed by a separate Python script, not by this code. Day 3's decoder must match it. */
#define GOLDEN_CHECKSUM 0x223c

extern unsigned g_fake_snapshots;

static int g_failures, g_checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                            \
        }                                                                            \
    } while (0)

static const hpm_snapshot_t SNAP = {
    .mcycle = 0x2222222222222222, .minstret = 0x3333333333333333,
    .hpm = { 0x4444444444444444, 0x5555555555555555, 0x6666666666666666 },
};

static sample_t make_sample(uint32_t seq, uint64_t base)
{
    hpm_snapshot_t snap = { .mcycle = base + 10, .minstret = base + 20,
                            .hpm = { base + 1, base + 2, base + 3 } };
    sample_t s;
    memset(&s, 0, sizeof(s));
    sample_fill(&s, seq, 2, &snap, base + 30, 0x80001234);
    return s;
}

static void test_layout(void)
{
    CHECK(sizeof(sample_t) == 64);
    CHECK(_Alignof(sample_t) == 64);
    CHECK(offsetof(sample_t, seq) == 0);
    CHECK(offsetof(sample_t, hart) == 4);
    CHECK(offsetof(sample_t, flags) == 5);
    CHECK(offsetof(sample_t, check) == 6);
    CHECK(offsetof(sample_t, mtime) == 8);
    CHECK(offsetof(sample_t, mcycle) == 16);
    CHECK(offsetof(sample_t, minstret) == 24);
    CHECK(offsetof(sample_t, hpm) == 32);
    CHECK(offsetof(sample_t, mepc) == 56);
}

static void test_fill_copies_every_field(void)
{
    sample_t s;
    memset(&s, 0xFF, sizeof(s));
    sample_fill(&s, 0x01020304, 2, &SNAP, 0x1111111111111111, 0x80001234);

    CHECK(s.seq == 0x01020304);
    CHECK(s.hart == 2);
    CHECK(s.flags == 0);
    CHECK(s.mtime == 0x1111111111111111);
    CHECK(s.mcycle == SNAP.mcycle);
    CHECK(s.minstret == SNAP.minstret);
    CHECK(s.hpm[0] == SNAP.hpm[0]);
    CHECK(s.hpm[1] == SNAP.hpm[1]);
    CHECK(s.hpm[2] == SNAP.hpm[2]);
    CHECK(s.mepc == 0x80001234);
#if SAMPLE_CHECKSUM
    CHECK(s.check == sample_checksum(&s));
#else
    CHECK(s.check == 0);
#endif
}

#if SAMPLE_CHECKSUM
static void test_checksum_golden(void)
{
    sample_t s;
    memset(&s, 0, sizeof(s));
    sample_fill(&s, 0x01020304, 2, &SNAP, 0x1111111111111111, 0x80001234);
    s.flags = 0x5A;
    CHECK(sample_checksum(&s) == GOLDEN_CHECKSUM);
}

/* Every one of the 512 bits, including those in check itself. */
static void test_checksum_catches_every_single_bit_flip(void)
{
    sample_t good = make_sample(7, 1000);

    for (unsigned bit = 0; bit < 512; bit++) {
        sample_t bad = good;
        ((uint8_t *)&bad)[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        CHECK(sample_check_next(NULL, &bad, 2) & SAMPLE_ERR_CHECKSUM);
    }
}

static void test_checksum_catches_swapped_fields(void)
{
    sample_t s = make_sample(7, 1000);
    uint64_t t = s.mcycle;
    s.mcycle = s.minstret;
    s.minstret = t;
    CHECK(sample_check_next(NULL, &s, 2) & SAMPLE_ERR_CHECKSUM);
}
#else
static void test_corruption_is_not_detected_without_checksum(void)
{
    sample_t s = make_sample(7, 1000);
    s.mcycle ^= 1;
    CHECK(sample_check_next(NULL, &s, 2) == 0);     /* documents the cost of turning it off */
}
#endif

static void test_check_next_accepts_good_stream(void)
{
    sample_t a = make_sample(7, 1000), b = make_sample(8, 2000);
    CHECK(sample_check_next(NULL, &a, 2) == 0);
    CHECK(sample_check_next(&a, &b, 2) == 0);

    sample_t last = make_sample(0xFFFFFFFF, 1000), first = make_sample(0, 2000);
    CHECK(sample_check_next(&last, &first, 2) == 0);    /* seq wrap is not a gap */
}

/* After editing a field on purpose, make the record look genuine again. */
static void reseal(sample_t *s)
{
#if SAMPLE_CHECKSUM
    s->check = sample_checksum(s);
#else
    (void)s;
#endif
}

static void test_check_next_rejects_bad_stream(void)
{
    sample_t a = make_sample(7, 1000);

    sample_t gap = make_sample(9, 2000);
    CHECK(sample_check_next(&a, &gap, 2) == SAMPLE_ERR_SEQ);

    sample_t repeat = make_sample(7, 2000);
    CHECK(sample_check_next(&a, &repeat, 2) == SAMPLE_ERR_SEQ);

    sample_t b = make_sample(8, 2000);
    CHECK(sample_check_next(&a, &b, 3) == SAMPLE_ERR_HART);

    sample_t back = make_sample(8, 2000);
    back.mtime = a.mtime - 1;
    reseal(&back);
    CHECK(sample_check_next(&a, &back, 2) == SAMPLE_ERR_MTIME);

    back = make_sample(8, 2000);
    back.mcycle = a.mcycle - 1;
    reseal(&back);
    CHECK(sample_check_next(&a, &back, 2) == SAMPLE_ERR_MCYCLE);

    back = make_sample(8, 2000);
    back.minstret = a.minstret - 1;
    reseal(&back);
    CHECK(sample_check_next(&a, &back, 2) == SAMPLE_ERR_MINSTRET);
}

static ring_t *make_ring(uint64_t capacity)
{
    ring_t *r = aligned_alloc(64, RING_BYTES(capacity));
    ring_init(r, capacity);
    return r;
}

static void test_tick_uses_backend_and_the_right_hart(void)
{
    ring_t *a = make_ring(2), *b = make_ring(2);

    g_fake_snapshots = 0;
    sampler_init(1, a);
    sampler_init(2, b);
    sampler_tick(1, 500, 0x80000010);
    sampler_tick(2, 600, 0x80000020);
    sampler_tick(1, 700, 0x80000030);

    CHECK(g_fake_snapshots == 3);
    CHECK(ring_occupancy(a) == 2);
    CHECK(ring_occupancy(b) == 1);
    CHECK(a->slots[0].mcycle == 1000 && a->slots[0].hpm[2] == 7 && a->slots[0].mtime == 500);
    CHECK(b->slots[0].mcycle == 2000 && b->slots[0].hart == 2 && b->slots[0].mepc == 0x80000020);
    CHECK(a->slots[1].minstret == 2700 && a->slots[1].seq == 1);
    CHECK(sample_check_next(&a->slots[0], &a->slots[1], 1) == 0);
    free(a);
    free(b);
}

static void test_tick_when_full_counts_a_drop_and_skips_the_snapshot(void)
{
    ring_t *r = make_ring(1);

    g_fake_snapshots = 0;
    sampler_init(1, r);
    sampler_tick(1, 100, 0);
    sampler_tick(1, 200, 0);
    sampler_tick(1, 300, 0);

    CHECK(g_fake_snapshots == 1);
    CHECK(ring_dropped(r) == 2);
    CHECK(sampler_attempts(1) == 3);        /* dropped samples still used a seq number */
    CHECK(r->slots[0].mtime == 100);
    ring_consume(r);
    sampler_tick(1, 400, 0);
    CHECK(r->slots[0].seq == 3);            /* the gap in seq shows the 2 drops */
    free(r);
}

#define RUN(t) do { printf("  %s\n", #t); t(); } while (0)

int main(void)
{
    printf("host unit tests (backend: %s, SAMPLE_CHECKSUM=%d)\n", hpm_backend_name(), SAMPLE_CHECKSUM);
    RUN(test_layout);
    RUN(test_fill_copies_every_field);
#if SAMPLE_CHECKSUM
    RUN(test_checksum_golden);
    RUN(test_checksum_catches_every_single_bit_flip);
    RUN(test_checksum_catches_swapped_fields);
#else
    RUN(test_corruption_is_not_detected_without_checksum);
#endif
    RUN(test_check_next_accepts_good_stream);
    RUN(test_check_next_rejects_bad_stream);
    RUN(test_tick_uses_backend_and_the_right_hart);
    RUN(test_tick_when_full_counts_a_drop_and_skips_the_snapshot);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures != 0;
}
