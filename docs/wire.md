# wire -- turning records into bytes

## What it does

`fw/src/wire.c` packs up to 16 records from one hart into a frame and makes that frame safe to send over a serial line.
It is pure code with no hardware access. The exact format is in `docs/DESIGN.md`.

Each step solves one problem:

| Step | Problem it solves |
| --- | --- |
| Delta | Counters are huge but change little, so send the change |
| Zigzag | A change can be negative (`mepc` moves backwards); this keeps small negatives small |
| Varint | Small numbers should take few bytes: 7 bits per byte, top bit means "more follows" |
| CRC-32 | Detects a damaged frame |
| COBS | Removes every `0x00`, so `0x00` can mark the end of a frame |

## Why this design

- **Frames stand alone.** The first sample in a frame is sent in full, and deltas restart in each frame. A lost frame then
  costs only that frame. The price is measured: 15.84 bytes/sample at 16 samples per frame, 14.81 at 64, 20.23 at 4
  (1 kHz, `-icount`). 16 keeps most of the saving while limiting what one lost frame costs.
- **`0x00` is reserved for frame ends.** After any damage, the decoder skips to the next `0x00` and is back in step.
- **`flags` and `check` are not sent.** `flags` is reserved, and the frame CRC protects the wire.
- **The CRC uses a 16-entry table.** That is 64 bytes of table, against 1 KiB for the usual 256 entries. Its CPU cost is
  not measured.

## What could go wrong

- **Worst-case size.** A frame of huge deltas must still fit its buffer. `test_worst_case_frame_fits` checks this.
- **COBS at exactly 254 bytes.** The classic edge case. Tests cover 254 and 255 byte runs.
- **CRC-32 is not perfect.** A damaged frame can pass with a chance of about 1 in 4 billion.
