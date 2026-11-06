/*
 * test_wire.c -- host tests for the encoder (make host-test). Hand-checked vectors here;
 * host/test_decoder.py then checks the C encoder against the Python decoder.
 */
#include <stdio.h>
#include <string.h>
#include "wire.h"

static int g_failures, g_checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        g_checks++;                                                                  \
        if (!(cond)) {                                                               \
            printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            g_failures++;                                                            \
        }                                                                            \
    } while (0)

static void test_zigzag(void)
{
    CHECK(wire_zigzag(0) == 0);
    CHECK(wire_zigzag(-1) == 1);
    CHECK(wire_zigzag(1) == 2);
    CHECK(wire_zigzag(-2) == 3);
    CHECK(wire_zigzag(2147483647) == 4294967294);
    CHECK(wire_zigzag(INT64_MAX) == UINT64_MAX - 1);
    CHECK(wire_zigzag(INT64_MIN) == UINT64_MAX);
}

static void test_varint(void)
{
    uint8_t b[WIRE_VARINT_MAX];

    CHECK(wire_put_varint(b, 0) == 1 && b[0] == 0);
    CHECK(wire_put_varint(b, 127) == 1 && b[0] == 127);
    CHECK(wire_put_varint(b, 128) == 2 && b[0] == 0x80 && b[1] == 1);
    CHECK(wire_put_varint(b, 300) == 2 && b[0] == 0xAC && b[1] == 0x02);
    CHECK(wire_put_varint(b, UINT64_MAX) == WIRE_VARINT_MAX && b[9] == 1);
}

static void test_crc(void)
{
    CHECK(wire_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926);   /* the standard check value */
    CHECK(wire_crc32((const uint8_t *)"", 0) == 0);
}

static void cobs_case(const uint8_t *in, size_t n, const uint8_t *want, size_t want_n)
{
    uint8_t out[600];
    size_t got = wire_cobs_encode(in, n, out);
    CHECK(got == want_n);
    CHECK(memcmp(out, want, want_n) == 0);
    for (size_t i = 0; i < got; i++)
        CHECK(out[i] != 0);
}

static void test_cobs_published_examples(void)
{
    cobs_case((const uint8_t[]){ 0x00 }, 1, (const uint8_t[]){ 0x01, 0x01 }, 2);
    cobs_case((const uint8_t[]){ 0x00, 0x00 }, 2, (const uint8_t[]){ 0x01, 0x01, 0x01 }, 3);
    cobs_case((const uint8_t[]){ 0x11, 0x22, 0x00, 0x33 }, 4,
              (const uint8_t[]){ 0x03, 0x11, 0x22, 0x02, 0x33 }, 5);
    cobs_case((const uint8_t[]){ 0x11, 0x22, 0x33, 0x44 }, 4,
              (const uint8_t[]){ 0x05, 0x11, 0x22, 0x33, 0x44 }, 5);
    cobs_case((const uint8_t[]){ 0x11, 0x00, 0x00, 0x00 }, 4,
              (const uint8_t[]){ 0x02, 0x11, 0x01, 0x01, 0x01 }, 5);
}

/* The boundary cases: exactly 254 and 255 non-zero bytes. */
static void test_cobs_long_runs(void)
{
    uint8_t in[255], want[258];

    memset(in, 0x41, sizeof(in));
    want[0] = 0xFF;
    memset(&want[1], 0x41, 254);
    cobs_case(in, 254, want, 255);

    want[255] = 0x02;
    want[256] = 0x41;
    cobs_case(in, 255, want, 257);
}

static void test_frame_has_no_zero_until_the_end(void)
{
    wire_frame_t f;
    uint8_t out[WIRE_MAX_FRAME];
    sample_t s;
    hpm_snapshot_t snap = { .mcycle = 0, .minstret = 256, .hpm = { 0, 65536, 0 } };

    memset(&s, 0, sizeof(s));
    sample_fill(&s, 0, 1, &snap, 0, 0x80000000);
    wire_begin(&f, WIRE_TYPE_SAMPLES, 1, 0, 0);
    wire_add_sample(&f, &s);
    size_t n = wire_finish(&f, out);

    CHECK(out[n - 1] == 0);
    for (size_t i = 0; i + 1 < n; i++)
        CHECK(out[i] != 0);
}

/* The worst case must fit: every field at maximum varint length. */
static void test_worst_case_frame_fits(void)
{
    static wire_frame_t f;
    static uint8_t out[WIRE_MAX_FRAME + 64];
    sample_t a, b;
    hpm_snapshot_t lo = { 0 }, hi = { .mcycle = 1ULL << 63, .minstret = 1ULL << 63,
                                      .hpm = { 1ULL << 63, 1ULL << 63, 1ULL << 63 } };

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    sample_fill(&a, 0, 1, &lo, 0, 0);
    sample_fill(&b, 0x80000000u, 1, &hi, 1ULL << 63, 1ULL << 63);
    memset(out, 0xEE, sizeof(out));

    wire_begin(&f, WIRE_TYPE_SAMPLES, 1, UINT64_MAX, UINT64_MAX);
    for (int i = 0; i < FRAME_SAMPLES; i++)
        wire_add_sample(&f, (i & 1) ? &a : &b);

    CHECK(f.len + 4 <= WIRE_MAX_PAYLOAD);
    size_t n = wire_finish(&f, out);
    CHECK(n <= WIRE_MAX_FRAME);
    CHECK(out[WIRE_MAX_FRAME] == 0xEE);
}

#define RUN(t) do { printf("  %s\n", #t); t(); } while (0)

int main(void)
{
    printf("wire unit tests\n");
    RUN(test_zigzag);
    RUN(test_varint);
    RUN(test_crc);
    RUN(test_cobs_published_examples);
    RUN(test_cobs_long_runs);
    RUN(test_frame_has_no_zero_until_the_end);
    RUN(test_worst_case_frame_fits);
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures != 0;
}
