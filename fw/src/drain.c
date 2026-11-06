/*
 * drain.c -- runs on the E51 only. All the expensive work lives here, off the U54s.
 *
 * Frames wait until FRAME_SAMPLES records are ready, because each frame pays for one absolute
 * sample. The cost is latency: at 1 kHz a frame represents 16 ms of samples.
 */
#include "drain.h"
#include "platform.h"
#include "uart.h"
#include "wire.h"

static struct {
    ring_t *ring;
    uint64_t frame_seq;     /* lets the decoder see a lost frame */
    uint64_t samples;
    uint64_t digest;        /* running digest of raw records, compared with the decoder's */
} g_drain[NUM_HARTS];

static drain_hook_t g_hook;
static uint64_t g_frames, g_bytes;

/* Static: the E51 stack is small, and only this one hart ever uses them. */
static wire_frame_t g_frame;
static uint8_t g_out[WIRE_MAX_FRAME];

void drain_init(ring_t *const rings[NUM_HARTS], drain_hook_t hook)
{
    for (unsigned h = 0; h < NUM_HARTS; h++)
        g_drain[h].ring = rings[h];
    g_hook = hook;
    uart_init(UART1_BASE);
}

static void send_frame(void)
{
    size_t len = wire_finish(&g_frame, g_out);

    for (size_t i = 0; i < len; i++)
        uart_putc(UART1_BASE, (char)g_out[i]);
    g_frames++;
    g_bytes += len;
}

static void send_samples(unsigned hart, unsigned count)
{
    ring_t *ring = g_drain[hart].ring;

    wire_begin(&g_frame, WIRE_TYPE_SAMPLES, (uint8_t)hart, g_drain[hart].frame_seq++, ring_dropped(ring));
    for (unsigned i = 0; i < count; i++) {
        const sample_t *s = ring_peek(ring);

        if (g_hook)
            g_hook(hart, s);
#if SAMPLE_CHECKSUM
        g_drain[hart].digest = sample_digest_step(g_drain[hart].digest, s);
#endif
        wire_add_sample(&g_frame, s);
        ring_consume(ring);
    }
    g_drain[hart].samples += count;
    send_frame();
}

void drain_poll(int flush)
{
    for (unsigned h = FIRST_APP_HART; h < NUM_HARTS; h++) {
        uint64_t ready;

        while ((ready = ring_occupancy(g_drain[h].ring)) >= FRAME_SAMPLES)
            send_samples(h, FRAME_SAMPLES);
        if (flush && ready)
            send_samples(h, (unsigned)ready);
    }
}

void drain_finish(unsigned hart, uint64_t attempts, uint64_t bad)
{
    wire_begin(&g_frame, WIRE_TYPE_END, (uint8_t)hart, g_drain[hart].frame_seq++,
               ring_dropped(g_drain[hart].ring));
    wire_add_end(&g_frame, attempts, g_drain[hart].samples, bad, SAMPLE_CHECKSUM, g_drain[hart].digest);
    send_frame();
}

uint64_t drain_samples(unsigned hart)
{
    return g_drain[hart].samples;
}

uint64_t drain_frames(void)
{
    return g_frames;
}

uint64_t drain_bytes(void)
{
    return g_bytes;
}
