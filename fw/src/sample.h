/*
 * sample.h -- the telemetry record: one 64-byte sample per timer tick per U54.
 *
 * Exactly one cache line, so a record never straddles two lines. See docs/sampler.md.
 * Pure data and pure functions: no CSRs, so all of this is unit-tested on the host.
 */
#ifndef HW_SAMPLE_H
#define HW_SAMPLE_H

#include <stddef.h>
#include <stdint.h>
#include "hpm_backend.h"

typedef struct {
    uint32_t seq;               /* counts every sample taken, including dropped ones      */
    uint8_t  hart;
    uint8_t  flags;             /* reserved, 0                                            */
    uint16_t check;             /* sample_checksum() if SAMPLE_CHECKSUM, otherwise 0       */
    uint64_t mtime;             /* sample time on the clock shared by all harts           */
    uint64_t mcycle;
    uint64_t minstret;
    uint64_t hpm[HPM_SLOTS];
    uint64_t mepc;              /* the instruction the timer interrupted                  */
} __attribute__((aligned(64))) sample_t;

_Static_assert(sizeof(sample_t) == 64, "sample_t must be exactly one cache line");
_Static_assert(offsetof(sample_t, check) == 6, "wire layout changed");
_Static_assert(offsetof(sample_t, mtime) == 8, "wire layout changed");
_Static_assert(offsetof(sample_t, hpm) == 32, "wire layout changed");
_Static_assert(offsetof(sample_t, mepc) == 56, "wire layout changed");

/* Reasons sample_check_next() can reject a record. */
#define SAMPLE_ERR_CHECKSUM  (1u << 0)
#define SAMPLE_ERR_HART      (1u << 1)
#define SAMPLE_ERR_SEQ       (1u << 2)
#define SAMPLE_ERR_MTIME     (1u << 3)
#define SAMPLE_ERR_MCYCLE    (1u << 4)
#define SAMPLE_ERR_MINSTRET  (1u << 5)

uint16_t sample_checksum(const sample_t *s);

void sample_fill(sample_t *s, uint32_t seq, uint8_t hart, const hpm_snapshot_t *snap,
                 uint64_t mtime, uint64_t mepc);

/* Validate cur, and its order after prev (prev may be NULL for the first record).
 * Returns 0 if good, otherwise SAMPLE_ERR_* bits. */
unsigned sample_check_next(const sample_t *prev, const sample_t *cur, uint8_t hart);

#endif
