/*
 * hpm_qemu.c -- counter backend for QEMU sifive_u (measured on QEMU 8.2.2, see docs/hpm_backend.md).
 *
 * WARNING: under -icount, mcycle and minstret are one global virtual clock, not per-hart
 * counts. minstret only equals this hart's instructions when every other hart is asleep.
 * The three events count fills of QEMU's software TLB, not hardware TLB misses.
 */
#include "hpm_backend.h"
#include "csr.h"

/* Event ids from QEMU's PMU code, confirmed by the counter probe. */
#define QEMU_EVENT_DTLB_READ_MISS   0x10019
#define QEMU_EVENT_DTLB_WRITE_MISS  0x1001B
#define QEMU_EVENT_ITLB_MISS        0x10021

const char *hpm_backend_name(void)
{
    return "qemu";
}

void hpm_init(void)
{
    /* Write 0 first: QEMU keeps an old selector counting until 0 clears it. */
    csr_write(mhpmevent3, 0);
    csr_write(mhpmevent4, 0);
    csr_write(mhpmevent5, 0);
    csr_write(mhpmevent3, QEMU_EVENT_DTLB_READ_MISS);
    csr_write(mhpmevent4, QEMU_EVENT_DTLB_WRITE_MISS);
    csr_write(mhpmevent5, QEMU_EVENT_ITLB_MISS);
    csr_write(mhpmcounter3, 0);
    csr_write(mhpmcounter4, 0);
    csr_write(mhpmcounter5, 0);
}

void hpm_snapshot(hpm_snapshot_t *out)
{
    out->mcycle   = csr_read(mcycle);
    out->minstret = csr_read(minstret);
    out->hpm[0]   = csr_read(mhpmcounter3);
    out->hpm[1]   = csr_read(mhpmcounter4);
    out->hpm[2]   = csr_read(mhpmcounter5);
}
