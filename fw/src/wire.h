/*
 * wire.h -- encode records into framed bytes for UART1. Format spec: docs/DESIGN.md.
 *
 * Pure functions with no hardware access, so the same code is tested on the host.
 *
 *   wire_begin(&f, ...);  wire_add_sample(&f, s) up to FRAME_SAMPLES times;
 *   len = wire_finish(&f, out);       out now holds COBS bytes ending in 0x00
 */
#ifndef HW_WIRE_H
#define HW_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include "sample.h"

#define WIRE_TYPE_SAMPLES  1
#define WIRE_TYPE_END      2

#define WIRE_FIELDS        8        /* seq, mtime, mcycle, minstret, hpm[3], mepc */
#define WIRE_VARINT_MAX    10       /* a 64-bit value needs at most 10 seven-bit groups */
#define WIRE_HEADER_MAX    (2 + 2 * WIRE_VARINT_MAX)
#define WIRE_MAX_PAYLOAD   (WIRE_HEADER_MAX + FRAME_SAMPLES * WIRE_FIELDS * WIRE_VARINT_MAX + 4)
#define WIRE_MAX_FRAME     (WIRE_MAX_PAYLOAD + WIRE_MAX_PAYLOAD / 254 + 2 + 1)

typedef struct {
    uint8_t buf[WIRE_MAX_PAYLOAD];
    size_t len;
    uint64_t prev[WIRE_FIELDS];     /* previous sample in this frame; zero at the start */
} wire_frame_t;

uint64_t wire_zigzag(int64_t v);
size_t wire_put_varint(uint8_t *out, uint64_t v);
uint32_t wire_crc32(const uint8_t *data, size_t len);
size_t wire_cobs_encode(const uint8_t *in, size_t len, uint8_t *out);

void wire_begin(wire_frame_t *f, uint8_t type, uint8_t hart, uint64_t frame_seq, uint64_t dropped);
void wire_add_sample(wire_frame_t *f, const sample_t *s);
void wire_add_end(wire_frame_t *f, uint64_t attempts, uint64_t drained, uint64_t bad,
                  uint8_t has_digest, uint64_t digest);
size_t wire_finish(wire_frame_t *f, uint8_t *out);

#endif
