/*
 * hpm_fake.c -- a backend for host tests: predictable values instead of CSRs.
 */
#include "hpm_backend.h"

unsigned g_fake_snapshots;

void hpm_init(void)
{
}

const char *hpm_backend_name(void)
{
    return "fake";
}

void hpm_snapshot(hpm_snapshot_t *out)
{
    uint64_t n = ++g_fake_snapshots;

    out->mcycle = n * 1000;
    out->minstret = n * 900;
    out->hpm[0] = n * 3;
    out->hpm[1] = n * 5;
    out->hpm[2] = n * 7;
}
