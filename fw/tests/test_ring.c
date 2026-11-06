/*
 * test_ring.c -- single-threaded ring tests (make host-test). These check the bookkeeping:
 * full, empty, wrap-around and drops. They say nothing about memory ordering; see
 * test_ring_tsan.c and docs/ring.md for that.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ring.h"

static int g_failures, g_checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                            \
        }                                                                            \
    } while (0)

static ring_t *make_ring(uint64_t capacity)
{
    ring_t *r = aligned_alloc(64, RING_BYTES(capacity));
    memset(r, 0xA5, RING_BYTES(capacity));
    CHECK(ring_init(r, capacity) == 0);
    return r;
}

static int push(ring_t *r, uint32_t seq)
{
    sample_t *s = ring_reserve(r);
    if (!s)
        return 0;
    s->seq = seq;
    ring_commit(r);
    return 1;
}

static void test_layout(void)
{
    CHECK(offsetof(ring_t, head) == 0);
    CHECK(offsetof(ring_t, dropped) == 8);
    CHECK(offsetof(ring_t, tail) == 64);
    CHECK(offsetof(ring_t, slots) % 64 == 0);
    CHECK(_Alignof(ring_t) == 64);
}

static void test_init_rejects_bad_capacity(void)
{
    static ring_t r;
    CHECK(ring_init(&r, 0) == -1);
    CHECK(ring_init(&r, 3) == -1);
    CHECK(ring_init(&r, 4096 + 1) == -1);
    CHECK(ring_init(&r, 1) == 0);
}

static void test_empty(void)
{
    ring_t *r = make_ring(4);
    CHECK(ring_peek(r) == NULL);
    CHECK(ring_occupancy(r) == 0);
    free(r);
}

static void test_fifo_order(void)
{
    ring_t *r = make_ring(4);
    for (uint32_t i = 0; i < 3; i++)
        CHECK(push(r, i));
    for (uint32_t i = 0; i < 3; i++) {
        const sample_t *s = ring_peek(r);
        CHECK(s && s->seq == i);
        ring_consume(r);
    }
    CHECK(ring_peek(r) == NULL);
    free(r);
}

static void test_peek_does_not_consume(void)
{
    ring_t *r = make_ring(4);
    push(r, 42);
    CHECK(ring_peek(r) == ring_peek(r));
    CHECK(ring_occupancy(r) == 1);
    free(r);
}

static void test_reserve_without_commit_publishes_nothing(void)
{
    ring_t *r = make_ring(4);
    CHECK(ring_reserve(r) != NULL);
    CHECK(ring_peek(r) == NULL);
    free(r);
}

static void test_every_slot_is_usable(void)
{
    ring_t *r = make_ring(4);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(push(r, i));
    CHECK(ring_occupancy(r) == 4);
    CHECK(ring_dropped(r) == 0);
    free(r);
}

static void test_full_drops_and_keeps_old_records(void)
{
    ring_t *r = make_ring(4);
    for (uint32_t i = 0; i < 4; i++)
        push(r, i);
    CHECK(push(r, 100) == 0);
    CHECK(push(r, 101) == 0);
    CHECK(ring_dropped(r) == 2);
    CHECK(ring_occupancy(r) == 4);
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(ring_peek(r)->seq == i);      /* old data untouched */
        ring_consume(r);
    }
    free(r);
}

static void test_space_returns_after_consume(void)
{
    ring_t *r = make_ring(2);
    push(r, 1);
    push(r, 2);
    CHECK(push(r, 3) == 0);
    ring_consume(r);
    CHECK(push(r, 4) == 1);
    CHECK(ring_dropped(r) == 1);
    free(r);
}

/* Many laps round the array, with the fill level changing all the time. */
static void test_wraps_many_times(void)
{
    ring_t *r = make_ring(8);
    uint32_t next_in = 0, next_out = 0;

    for (int round = 0; round < 100000; round++) {
        for (int i = 0; i < (round % 5); i++)
            if (push(r, next_in))
                next_in++;
        for (int i = 0; i < (round % 3) + 1; i++) {
            const sample_t *s = ring_peek(r);
            if (!s)
                break;
            if (s->seq != next_out)
                CHECK(s->seq == next_out);
            next_out++;
            ring_consume(r);
        }
    }
    CHECK(next_out > 100000);
    CHECK(r->head == next_in);
    free(r);
}

/* The counters must survive passing a large value, where a wrapped index would go wrong. */
static void test_indices_near_the_top_of_their_range(void)
{
    ring_t *r = make_ring(4);
    r->head = r->tail = ~0ULL - 2;
    for (uint32_t i = 0; i < 4; i++)
        CHECK(push(r, i));
    CHECK(push(r, 9) == 0);
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(ring_peek(r)->seq == i);
        ring_consume(r);
    }
    CHECK(ring_peek(r) == NULL);
    free(r);
}

#define RUN(t) do { printf("  %s\n", #t); t(); } while (0)

int main(void)
{
    printf("ring unit tests\n");
    RUN(test_layout);
    RUN(test_init_rejects_bad_capacity);
    RUN(test_empty);
    RUN(test_fifo_order);
    RUN(test_peek_does_not_consume);
    RUN(test_reserve_without_commit_publishes_nothing);
    RUN(test_every_slot_is_usable);
    RUN(test_full_drops_and_keeps_old_records);
    RUN(test_space_returns_after_consume);
    RUN(test_wraps_many_times);
    RUN(test_indices_near_the_top_of_their_range);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures != 0;
}
