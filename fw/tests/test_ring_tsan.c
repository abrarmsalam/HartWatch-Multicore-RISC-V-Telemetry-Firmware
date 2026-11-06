/*
 * test_ring_tsan.c -- one producer thread, one consumer thread, under ThreadSanitizer.
 *
 * TSan reports any access pair not ordered by the C11 model, so a missing acquire or release
 * shows as a data race on the slot, even if the data happened to arrive intact.
 * What this cannot show: anything about RISC-V machine code. See docs/ring.md.
 *
 * usage: test_ring_tsan <attempts> <capacity>
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ring.h"

static ring_t *g_ring;
static uint64_t g_attempts;
static volatile int g_producer_done;

/* Every field is derived from seq, so the consumer can recompute the whole record. */
static void fill(sample_t *s, uint32_t seq)
{
    hpm_snapshot_t snap = {
        .mcycle = seq * 3ULL, .minstret = seq * 5ULL,
        .hpm = { seq * 7ULL, seq * 11ULL, seq * 13ULL },
    };
    sample_fill(s, seq, 1, &snap, seq * 17ULL, seq * 19ULL);
}

static void *producer(void *arg)
{
    (void)arg;
    for (uint64_t i = 0; i < g_attempts; i++) {
        sample_t *s = ring_reserve(g_ring);
        if (s) {
            fill(s, (uint32_t)i);
            ring_commit(g_ring);
        }
    }
    __atomic_store_n(&g_producer_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(int argc, char **argv)
{
    uint64_t capacity = argc > 2 ? strtoull(argv[2], NULL, 0) : 64;
    uint64_t received = 0, gaps = 0, bad = 0;
    int64_t last = -1;
    pthread_t thread;

    g_attempts = argc > 1 ? strtoull(argv[1], NULL, 0) : 1000000;
    g_ring = aligned_alloc(64, RING_BYTES(capacity));
    if (ring_init(g_ring, capacity))
        return 2;
    pthread_create(&thread, NULL, producer, NULL);

    for (;;) {
        int done = __atomic_load_n(&g_producer_done, __ATOMIC_ACQUIRE);
        const sample_t *s = ring_peek(g_ring);
        if (!s) {
            if (done)
                break;
            continue;
        }
        sample_t want;
        memset(&want, 0, sizeof(want));
        fill(&want, s->seq);
        if (memcmp(s, &want, sizeof(want)) || (int64_t)s->seq <= last)
            bad++;
        gaps += (uint64_t)((int64_t)s->seq - last - 1);
        last = s->seq;
        received++;
        ring_consume(g_ring);
    }
    pthread_join(thread, NULL);

    uint64_t dropped = ring_dropped(g_ring);
    uint64_t tail_lost = g_attempts - 1 - (uint64_t)last;
    printf("attempts %lu received %lu dropped %lu seq_gaps %lu bad %lu\n",
           (unsigned long)g_attempts, (unsigned long)received, (unsigned long)dropped,
           (unsigned long)(gaps + tail_lost), (unsigned long)bad);
    free(g_ring);
    return bad != 0 || received + dropped != g_attempts || gaps + tail_lost != dropped;
}
