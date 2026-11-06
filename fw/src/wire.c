/*
 * wire.c -- delta + zigzag + varint encoding, CRC-32 and COBS framing.
 *
 * Why these steps, in order:
 *   delta   counters change little between samples, so send the difference
 *   zigzag  a difference can be negative (mepc); this maps small +/- values to small numbers
 *   varint  small numbers take fewer bytes: 7 bits per byte, top bit means "more follows"
 *   CRC-32  detects a damaged frame
 *   COBS    removes every 0x00 byte, so 0x00 can mark the end of a frame unambiguously.
 *           After losing bytes, the decoder skips to the next 0x00 and is back in step.
 */
#include "wire.h"

uint64_t wire_zigzag(int64_t v)
{
    uint64_t u = (uint64_t)v;
    return (u << 1) ^ (0 - (u >> 63));
}

size_t wire_put_varint(uint8_t *out, uint64_t v)
{
    size_t n = 0;

    while (v >= 0x80) {
        out[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

/* CRC-32/ISO-HDLC, the same CRC as zlib. A 16-entry table, one lookup per nibble: 64 bytes
 * of table instead of 1 KiB, and about four times fewer steps than going bit by bit. */
uint32_t wire_crc32(const uint8_t *data, size_t len)
{
    static const uint32_t table[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    uint32_t crc = ~0u;

    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xF] ^ (crc >> 4);
        crc = table[(crc ^ (data[i] >> 4)) & 0xF] ^ (crc >> 4);
    }
    return ~crc;
}

/* Each block starts with a code byte: the distance to the next zero (0xFF: 254 data bytes
 * and no zero). `code_pos` is where that byte goes once the block length is known. */
size_t wire_cobs_encode(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t read = 0, write = 1, code_pos = 0;
    uint8_t code = 1;

    while (read < len) {
        if (in[read] == 0) {
            out[code_pos] = code;
            code = 1;
            code_pos = write++;
            read++;
        } else {
            out[write++] = in[read++];
            code++;
            if (code == 0xFF && read < len) {
                out[code_pos] = code;
                code = 1;
                code_pos = write++;
            }
        }
    }
    out[code_pos] = code;
    return write;
}

static void put_varint(wire_frame_t *f, uint64_t v)
{
    f->len += wire_put_varint(&f->buf[f->len], v);
}

void wire_begin(wire_frame_t *f, uint8_t type, uint8_t hart, uint64_t frame_seq, uint64_t dropped)
{
    f->buf[0] = type;
    f->buf[1] = hart;
    f->len = 2;
    put_varint(f, frame_seq);
    put_varint(f, dropped);
    for (unsigned i = 0; i < WIRE_FIELDS; i++)
        f->prev[i] = 0;
}

/* `flags` and `check` are not sent: flags is reserved, and the frame CRC protects the wire. */
void wire_add_sample(wire_frame_t *f, const sample_t *s)
{
    const uint64_t cur[WIRE_FIELDS] = {
        s->seq, s->mtime, s->mcycle, s->minstret, s->hpm[0], s->hpm[1], s->hpm[2], s->mepc,
    };

    for (unsigned i = 0; i < WIRE_FIELDS; i++) {
        put_varint(f, wire_zigzag((int64_t)(cur[i] - f->prev[i])));
        f->prev[i] = cur[i];
    }
}

void wire_add_end(wire_frame_t *f, uint64_t attempts, uint64_t drained, uint64_t bad,
                  uint8_t has_digest, uint64_t digest)
{
    put_varint(f, attempts);
    put_varint(f, drained);
    put_varint(f, bad);
    f->buf[f->len++] = has_digest;
    put_varint(f, digest);
}

size_t wire_finish(wire_frame_t *f, uint8_t *out)
{
    uint32_t crc = wire_crc32(f->buf, f->len);

    for (unsigned i = 0; i < 4; i++)
        f->buf[f->len++] = (uint8_t)(crc >> (8 * i));

    size_t n = wire_cobs_encode(f->buf, f->len, out);
    out[n++] = 0;
    return n;
}
