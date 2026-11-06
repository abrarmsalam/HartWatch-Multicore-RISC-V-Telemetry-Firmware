/*
 * sampler.c -- turn a timer tick into a stored record.
 */
#include "sampler.h"

static sample_log_t g_log[NUM_HARTS];

void sample_log_init(sample_log_t *log, uint8_t hart, sample_t *slots, uint32_t capacity)
{
    log->slots = slots;
    log->capacity = capacity;
    log->count = 0;
    log->next_seq = 0;
    log->hart = hart;
    log->dropped = 0;
}

void sample_log_record(sample_log_t *log, const hpm_snapshot_t *snap, uint64_t mtime, uint64_t mepc)
{
    uint32_t seq = log->next_seq++;

    if (log->count == log->capacity) {
        log->dropped++;
        return;
    }
    sample_fill(&log->slots[log->count], seq, log->hart, snap, mtime, mepc);
    log->count++;
}

void sampler_init(unsigned hart, sample_t *slots, uint32_t capacity)
{
    sample_log_init(&g_log[hart], (uint8_t)hart, slots, capacity);
}

void sampler_tick(unsigned hart, uint64_t mtime, uint64_t mepc)
{
    hpm_snapshot_t snap;

    hpm_snapshot(&snap);
    sample_log_record(&g_log[hart], &snap, mtime, mepc);
}

const sample_log_t *sampler_log(unsigned hart)
{
    return &g_log[hart];
}
