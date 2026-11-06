# drain -- the E51's job

## What it does

`fw/src/drain.c` runs only on the E51. Every 16 ms it wakes from `wfi` and, for each U54's ring, sends a frame whenever 16
records are waiting. When the run ends, it flushes partial frames and sends each hart's END frame with its final totals.

## Why this design

- **All the expensive work is here.** Encoding, CRC and UART writes happen on the E51, so the U54s pay only for the sample.
- **It sleeps between drains.** A spinning E51 would waste host CPU. Under `-icount` it would also push virtual time
  forward, since every instruction on any hart advances the clock.
- **It waits for full frames**, because each frame pays for one full sample. The cost is latency: 16 ms at 1 kHz, 160 ms
  at 100 Hz.
- **The drop counter rides in every frame**, so drops are visible as they happen, not only at the end.
- **END frames carry a digest of the raw records.** The decoder recomputes it from what it decoded, which proves the data
  survived the whole path. This is test instrumentation, and it is off with `SAMPLE_CHECKSUM=0`.

## What could go wrong

- **The E51 falls behind.** Rings fill and samples drop. That is the designed behaviour, and `make test-drops` proves the
  counts are right. The E51's maximum rate through the real encoder is not measured.
- **A UART that blocks.** QEMU's UART has no baud limit. On a real one, `uart_putc` would spin while the FIFO is full and the
  E51 would become the bottleneck. At 115200 baud a UART carries 11.52 KB/s (calculated), below the 63 KB/s measured at
  1 kHz. Real hardware would need a lower rate, a faster link, or fewer fields.
- **Sending END too early.** `drain_finish` must only run after the hart has stopped and its ring is empty.
