/*
 * sampler.h -- what the U54 timer ISR does on each tick.
 *
 * Day 2 stores records in a simple per-hart log. Day 3 replaces the log with the SPSC ring;
 * the drop-on-full rule is already the ring's rule: never block, never overwrite, count it.
 */
#ifndef HW_SAMPLER_H
#define HW_SAMPLER_H

#include <stdint.h>
#include "sample.h"

typedef struct {
    sample_t *slots;
    uint32_t capacity;
    uint32_t count;             /* records stored                                  */
    uint32_t next_seq;          /* advances on every sample, stored or dropped     */
    uint8_t  hart;
    uint64_t dropped;
} __attribute__((aligned(64))) sample_log_t;

void sample_log_init(sample_log_t *log, uint8_t hart, sample_t *slots, uint32_t capacity);
void sample_log_record(sample_log_t *log, const hpm_snapshot_t *snap, uint64_t mtime, uint64_t mepc);

/* Prepare the calling hart's log. Call before its timer starts. */
void sampler_init(unsigned hart, sample_t *slots, uint32_t capacity);

/* Take one sample. Called from the timer ISR, so it must never print or block. */
void sampler_tick(unsigned hart, uint64_t mtime, uint64_t mepc);

const sample_log_t *sampler_log(unsigned hart);

#endif
