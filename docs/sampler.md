# sample and sampler -- turning a timer tick into a record

## What it does

On every timer tick, each U54 does this inside the interrupt:

1. `timer_irq` reads the shared clock `mtime`.
2. `sampler_tick` asks `hpm_backend` for a counter snapshot.
3. `sampler_tick` fills one 64-byte `sample_t` in the ring, including `mepc`, the instruction that was interrupted.

`sample.c` builds and checks records. `sampler.c` puts them in the hart's ring (`docs/ring.md`). The E51 has a timer too,
but it only uses it to wake up and it does not sample.

Rings live in L2-LIM: 4096 slots of 64 bytes, 256 KiB per hart, about 1 MiB in total.

## Why this design

- **64 bytes is one cache line**, so a record never straddles two lines.
- **`mtime` is taken in the ISR**, not when the E51 drains the record. Drain time would be late by however long the
  record waited. `mtime` is the only clock all harts share, and `timer_irq` already reads it.
- **`seq` advances even when a sample is dropped.** A gap in `seq` therefore shows exactly how many were lost.
- **Drop when full, never block and never overwrite.** The hart being measured must not wait for telemetry.
- **No CSRs in `sample.c`**, so host tests can check it under AddressSanitizer and UBSan.
- **`check` is test instrumentation, not part of the telemetry design.** It exists so tests can catch corrupted or torn
  records. Build with `SAMPLE_CHECKSUM=0` and the ISR does no checksum work and `check` stays 0. Tests use 1; the overhead
  sweep must use 0, or it measures the test harness.
- **It is a cheap rotate-and-xor.** The rotate makes position matter, so two swapped fields are caught, which a plain
  xor would miss. It is not a strong hash: two flipped bits can cancel out.

## How it is tested

- `make test`: four U54s run different workloads and hart 0 validates every record: checksum, hart, `seq` order, and
  that `mtime`, `mcycle` and `minstret` never go backwards. It also checks that `mepc` points into the right workload and
  that read and write misses follow the workload.
- Bugs were planted on purpose to prove the tests can fail. See the Day 2 log.

## What could go wrong

- **The ISR gets too slow.** Each tick does a snapshot and 64 bytes of stores, plus a checksum in test builds. The cost
  is not measured yet. It is part of the Day 6 overhead sweep.
- **Reading a record while it is being written.** The ring's store-release and load-acquire prevent this. See
  `docs/ring.md`.
- **The ring fills up.** New samples are dropped and counted, and `seq` still advances, so the gap shows the loss.
