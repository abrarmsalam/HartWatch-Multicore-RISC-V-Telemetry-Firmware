/*
 * hpm_sifive.c -- counter backend for FU540 silicon. UNTESTED ON HARDWARE.
 *
 * This file compiles, and that is all that has been checked. Real IPC would come from here,
 * because on silicon mcycle and minstret are separate per-hart counters. Before trusting it,
 * check against the FU540 manual: the selector values below, and the number of event
 * counters. Both are from memory. As remembered, the U54 has only mhpmcounter3 and 4 and one
 * combined DTLB miss event, so slot 1 (DTLB write miss on QEMU) stays 0 here.
 */
#include "hpm_backend.h"
#include "csr.h"

/* SiFive encoding: event class in bits 7:0, event mask above it. Class 2 = memory system. */
#define SIFIVE_EVENT_ITLB_MISS  ((1UL << 11) | 2)
#define SIFIVE_EVENT_DTLB_MISS  ((1UL << 12) | 2)

const char *hpm_backend_name(void)
{
    return "sifive (untested on hardware)";
}

void hpm_init(void)
{
    csr_write(mhpmevent3, SIFIVE_EVENT_DTLB_MISS);
    csr_write(mhpmevent4, SIFIVE_EVENT_ITLB_MISS);
    csr_write(mhpmcounter3, 0);
    csr_write(mhpmcounter4, 0);
}

void hpm_snapshot(hpm_snapshot_t *out)
{
    out->mcycle   = csr_read(mcycle);
    out->minstret = csr_read(minstret);
    out->hpm[0]   = csr_read(mhpmcounter3);
    out->hpm[1]   = 0;
    out->hpm[2]   = csr_read(mhpmcounter4);
}
