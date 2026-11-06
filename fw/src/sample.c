/*
 * sample.c -- build and validate telemetry records.
 */
#include "sample.h"

static uint64_t rotl7(uint64_t x)
{
    return (x << 7) | (x >> 57);
}

/* Rotate-and-xor over the record's eight 64-bit words, folded to 16 bits. A plain xor would
 * miss two fields swapping places; the rotate makes position matter. It is cheap because it
 * runs in the ISR. This catches torn or corrupted records, it is not a strong hash. */
uint16_t sample_checksum(const sample_t *s)
{
    uint64_t acc = (uint64_t)s->seq | ((uint64_t)s->hart << 32) | ((uint64_t)s->flags << 40);

    acc = rotl7(acc) ^ s->mtime;
    acc = rotl7(acc) ^ s->mcycle;
    acc = rotl7(acc) ^ s->minstret;
    for (unsigned i = 0; i < HPM_SLOTS; i++)
        acc = rotl7(acc) ^ s->hpm[i];
    acc = rotl7(acc) ^ s->mepc;

    return (uint16_t)(acc ^ (acc >> 16) ^ (acc >> 32) ^ (acc >> 48));
}

void sample_fill(sample_t *s, uint32_t seq, uint8_t hart, const hpm_snapshot_t *snap,
                 uint64_t mtime, uint64_t mepc)
{
    s->seq = seq;
    s->hart = hart;
    s->flags = 0;
    s->mtime = mtime;
    s->mcycle = snap->mcycle;
    s->minstret = snap->minstret;
    for (unsigned i = 0; i < HPM_SLOTS; i++)
        s->hpm[i] = snap->hpm[i];
    s->mepc = mepc;
#if SAMPLE_CHECKSUM
    s->check = sample_checksum(s);
#else
    s->check = 0;
#endif
}

unsigned sample_check_next(const sample_t *prev, const sample_t *cur, uint8_t hart)
{
    unsigned err = 0;

#if SAMPLE_CHECKSUM
    if (cur->check != sample_checksum(cur))
        err |= SAMPLE_ERR_CHECKSUM;
#endif
    if (cur->hart != hart)
        err |= SAMPLE_ERR_HART;
    if (prev) {
        /* Unsigned subtraction, so seq wrapping from 0xFFFFFFFF to 0 is still "+1". */
        if ((uint32_t)(cur->seq - prev->seq) != 1)
            err |= SAMPLE_ERR_SEQ;
        if (cur->mtime < prev->mtime)
            err |= SAMPLE_ERR_MTIME;
        if (cur->mcycle < prev->mcycle)
            err |= SAMPLE_ERR_MCYCLE;
        if (cur->minstret < prev->minstret)
            err |= SAMPLE_ERR_MINSTRET;
    }
    return err;
}
