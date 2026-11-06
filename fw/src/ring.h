/*
 * ring.h -- lock-free single-producer/single-consumer sample ring.
 *
 * Read docs/ring.md first: it holds the invariants and the ordering argument this code
 * relies on. The producer is one U54's timer ISR, the consumer is the E51.
 *
 * Writing and reading happen in place, in two steps, so a record is never copied:
 *   producer:  s = ring_reserve(r);  ...fill *s...;  ring_commit(r);
 *   consumer:  s = ring_peek(r);     ...read *s...;  ring_consume(r);
 */
#ifndef HW_RING_H
#define HW_RING_H

#include <stddef.h>
#include <stdint.h>
#include "sample.h"

typedef struct {
    uint64_t head __attribute__((aligned(64)));     /* producer writes                 */
    uint64_t dropped;                               /* producer writes                 */
    uint64_t tail __attribute__((aligned(64)));     /* consumer writes                 */
    uint64_t mask __attribute__((aligned(64)));     /* capacity - 1, fixed after init  */
    sample_t slots[];
} ring_t;

_Static_assert(offsetof(ring_t, tail) - offsetof(ring_t, head) == 64, "head and tail must not share a line");
_Static_assert(offsetof(ring_t, mask) == 128, "mask must not share a line with tail");
_Static_assert(offsetof(ring_t, slots) == 192, "slots must start on their own line");

#define RING_BYTES(capacity) (offsetof(ring_t, slots) + (size_t)(capacity) * sizeof(sample_t))

/* capacity must be a power of two. Call before either side runs. Returns 0 on success. */
static inline int ring_init(ring_t *r, uint64_t capacity)
{
    if (capacity == 0 || (capacity & (capacity - 1)))
        return -1;
    r->head = 0;
    r->dropped = 0;
    r->tail = 0;
    r->mask = capacity - 1;
    return 0;
}

/* Producer: get the next free slot, or NULL if full (the sample is then counted as dropped).
 * The acquire pairs with the release in ring_consume: if we see the new tail, the consumer
 * has finished reading that slot, so we may overwrite it. */
static inline sample_t *ring_reserve(ring_t *r)
{
    uint64_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

    if (head - tail > r->mask) {
        /* A plain store is enough: only this side writes `dropped`, and it guards no data. */
        __atomic_store_n(&r->dropped, __atomic_load_n(&r->dropped, __ATOMIC_RELAXED) + 1,
                         __ATOMIC_RELAXED);
        return NULL;
    }
    return &r->slots[head & r->mask];
}

/* Producer: publish the slot from ring_reserve. The release pairs with the acquire in
 * ring_peek: whoever sees the new head also sees the finished record. */
static inline void ring_commit(ring_t *r)
{
    uint64_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    __atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE);
}

/* Consumer: the oldest published record, or NULL if empty. */
static inline const sample_t *ring_peek(ring_t *r)
{
    uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint64_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);

    return head == tail ? NULL : &r->slots[tail & r->mask];
}

/* Consumer: hand the slot from ring_peek back. The release keeps our reads of the record
 * before this store, so the producer cannot overwrite it while we read. */
static inline void ring_consume(ring_t *r)
{
    uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    __atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELEASE);
}

/* Records waiting. Consumer side: it can only be an underestimate, as the producer adds more. */
static inline uint64_t ring_occupancy(ring_t *r)
{
    uint64_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - tail;
}

static inline uint64_t ring_dropped(ring_t *r)
{
    return __atomic_load_n(&r->dropped, __ATOMIC_RELAXED);
}

#endif
