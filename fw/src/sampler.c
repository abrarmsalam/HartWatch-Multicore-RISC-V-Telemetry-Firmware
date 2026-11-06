/*
 * sampler.c -- turn a timer tick into a record in the hart's ring.
 */
#include "sampler.h"

static struct {
    ring_t *ring;
    uint32_t next_seq;      /* advances on every attempt, so a gap in seq shows a drop */
} __attribute__((aligned(64))) g_sampler[NUM_HARTS];

void sampler_init(unsigned hart, ring_t *ring)
{
    g_sampler[hart].ring = ring;
    g_sampler[hart].next_seq = 0;
}

void sampler_tick(unsigned hart, uint64_t mtime, uint64_t mepc)
{
    uint32_t seq = g_sampler[hart].next_seq++;
    sample_t *slot = ring_reserve(g_sampler[hart].ring);
    hpm_snapshot_t snap;

    if (!slot)
        return;             /* full: never block. ring_reserve counted the drop. */
    hpm_snapshot(&snap);
    sample_fill(slot, seq, (uint8_t)hart, &snap, mtime, mepc);
    ring_commit(g_sampler[hart].ring);
}

uint32_t sampler_attempts(unsigned hart)
{
    return g_sampler[hart].next_seq;
}
