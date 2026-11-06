/*
 * gen_vectors.c -- write frames from the C encoder plus the values that went in, so
 * host/test_decoder.py can prove the two languages agree.
 *
 * usage: gen_vectors <frames.bin> <expected.txt>
 * expected.txt: one line per sample: hart seq mtime mcycle minstret hpm0 hpm1 hpm2 mepc
 */
#include <stdio.h>
#include <string.h>
#include "wire.h"

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;

static uint64_t next_rand(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

/* Three kinds of value: small steps, large jumps, and the extremes of 64 bits. */
static uint64_t pick(uint64_t prev, unsigned kind)
{
    switch (kind % 3) {
    case 0:  return prev + next_rand() % 1000;
    case 1:  return next_rand();
    default: return (next_rand() & 1) ? UINT64_MAX : 0;
    }
}

int main(int argc, char **argv)
{
    static wire_frame_t f;
    static uint8_t out[WIRE_MAX_FRAME];
    uint64_t v[7] = { 0 };

    if (argc != 3)
        return 2;
    FILE *bin = fopen(argv[1], "wb"), *txt = fopen(argv[2], "w");
    if (!bin || !txt)
        return 2;

    for (uint64_t frame = 0; frame < 300; frame++) {
        uint8_t hart = 1 + frame % 4;
        unsigned n = 1 + frame % FRAME_SAMPLES;

        wire_begin(&f, WIRE_TYPE_SAMPLES, hart, frame, frame * 3);
        for (unsigned i = 0; i < n; i++) {
            hpm_snapshot_t snap;
            sample_t s;
            uint32_t seq = (uint32_t)(0xFFFFFF00u + frame * 16 + i);    /* wraps past 2^32 */

            for (unsigned k = 0; k < 7; k++)
                v[k] = pick(v[k], (unsigned)frame + k);
            snap.mcycle = v[1];
            snap.minstret = v[2];
            memcpy(snap.hpm, &v[3], sizeof(snap.hpm));
            memset(&s, 0, sizeof(s));
            sample_fill(&s, seq, hart, &snap, v[0], v[6]);
            wire_add_sample(&f, &s);
            fprintf(txt, "%u %u %llu %llu %llu %llu %llu %llu %llu\n", s.hart, s.seq,
                    (unsigned long long)s.mtime, (unsigned long long)s.mcycle,
                    (unsigned long long)s.minstret, (unsigned long long)s.hpm[0],
                    (unsigned long long)s.hpm[1], (unsigned long long)s.hpm[2],
                    (unsigned long long)s.mepc);
        }
        fwrite(out, 1, wire_finish(&f, out), bin);
    }
    fclose(bin);
    fclose(txt);
    return 0;
}
