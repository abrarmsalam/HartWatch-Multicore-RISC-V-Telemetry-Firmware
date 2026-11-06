/*
 * sampler.h -- what the U54 timer ISR does on each tick.
 */
#ifndef HW_SAMPLER_H
#define HW_SAMPLER_H

#include <stdint.h>
#include "ring.h"

/* Attach the calling hart to its ring. Call before its timer starts. */
void sampler_init(unsigned hart, ring_t *ring);

/* Take one sample. Called from the timer ISR, so it must never print or block. */
void sampler_tick(unsigned hart, uint64_t mtime, uint64_t mepc);

/* Samples attempted so far, stored or dropped. Read it only after the hart has stopped. */
uint32_t sampler_attempts(unsigned hart);

#endif
