# Host decoder

## What it does

`host/decoder.py` turns captured UART1 bytes back into records, using pure functions. `host/check_stream.py` then checks
a whole capture: `make test` runs it on `build/telemetry.bin` after every boot.

## Why this design

- **Pure functions** are easy to test: bytes in, values out.
- **Damaged frames are counted and skipped.** One bad frame should not lose the rest of the capture.
- **It does not trust the firmware.** It recomputes the digest, adds up `seq` gaps and compares them with the drop counter,
  and checks `frame_seq` for lost frames.
- **It is tested against the real C encoder.** `fw/tests/gen_vectors.c` writes frames and the values that went into them,
  and pytest compares. Two implementations that agree are better evidence than one tested against itself.

## What it checks in a capture

- No damaged frames, and no gaps in `frame_seq`.
- Decoded samples match the count the E51 says it sent.
- `drained + dropped == attempts`, and `seq` gaps add up to the drop counter.
- `seq` only moves forward, across its 32-bit wrap too, and counters never go backwards.
- The digest of decoded records matches the E51's digest of the raw records.

## What could go wrong

- **`seq` wraps at 2^32.** A first version of the check treated that as going backwards. The stress test exposed this.
- **Digest only exists with `SAMPLE_CHECKSUM=1`.** Without it, the decoder cannot prove the values are right, only that the
  frames are intact.
- **Python and C digests must stay identical.** Tests lock this to a value computed separately.
- **Speed.** It has only handled small captures so far. Its throughput is not measured.
