/*
 * hpm_backend.h -- the only interface the sampler uses to reach performance counters.
 *
 * Counters differ between QEMU and silicon, so each target gets its own implementation in
 * fw/src/hpm/, chosen at build time with HPM_BACKEND=qemu|sifive. Host unit tests link a
 * fake one. No CSR appears here, which is what lets the sampler compile on a laptop.
 */
#ifndef HW_HPM_BACKEND_H
#define HW_HPM_BACKEND_H

#include <stdint.h>
#include "config.h"

typedef struct {
    uint64_t mcycle;
    uint64_t minstret;
    uint64_t hpm[HPM_SLOTS];    /* 0 = DTLB read miss, 1 = DTLB write miss, 2 = ITLB miss */
} hpm_snapshot_t;

/* Program the event selectors. CSRs are per-hart, so each sampled hart calls this itself. */
void hpm_init(void);

/* Read the calling hart's counters. Runs in the ISR: no printing, no blocking. */
void hpm_snapshot(hpm_snapshot_t *out);

const char *hpm_backend_name(void);

#endif
