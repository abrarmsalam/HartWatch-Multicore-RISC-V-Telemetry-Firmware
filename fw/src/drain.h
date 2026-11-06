/*
 * drain.h -- the E51's job: empty the rings, encode the records, send frames on UART1.
 */
#ifndef HW_DRAIN_H
#define HW_DRAIN_H

#include <stdint.h>
#include "ring.h"

/* Called for every record as it leaves a ring. The self-test uses this to validate them. */
typedef void (*drain_hook_t)(unsigned hart, const sample_t *s);

void drain_init(ring_t *const rings[NUM_HARTS], drain_hook_t hook);

/* Send every full frame's worth of records. With flush set, send partial frames too. */
void drain_poll(int flush);

/* Send one hart's END frame with its final totals. Call once that hart has stopped and its
 * ring is empty. `bad` counts records the caller rejected. */
void drain_finish(unsigned hart, uint64_t attempts, uint64_t bad);

uint64_t drain_samples(unsigned hart);
uint64_t drain_frames(void);
uint64_t drain_bytes(void);

#endif
