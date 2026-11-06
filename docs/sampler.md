# sample and sampler -- turning a timer tick into a record

## What it does

On every timer tick, each U54 does this inside the interrupt:

1. `timer_irq` reads the shared clock `mtime`.
2. `sampler_tick` asks `hpm_backend` for a counter snapshot.
3. `sample_log_record` fills one 64-byte `sample_t`, including `mepc`, the instruction that was interrupted.

`sample.c` builds and checks records. `sampler.c` decides where they go. The E51 has a timer too, but it only uses it
to wake up and it does not sample.

Records go into a plain per-hart log in L2-LIM: 4096 slots of 64 bytes, 256 KiB per hart, 1 MiB in total. This log is
temporary. Day 3 replaces it with the single-producer/single-consumer ring.

## Why this design

- **64 bytes is one cache line**, so a record never straddles two lines.
- **`mtime` is taken in the ISR**, not when the E51 drains the record. Drain time would be late by however long the
  record waited. `mtime` is the only clock all harts share, and `timer_irq` already reads it.
- **`seq` advances even when a sample is dropped.** A gap in `seq` therefore shows exactly how many were lost.
- **Drop when full, never block and never overwrite.** The hart being measured must not wait for telemetry.
- **No CSRs in `sample.c`**, so host tests can check it under AddressSanitizer and UBSan.
- **`check` is a cheap rotate-and-xor.** The rotate makes position matter, so two swapped fields are caught, which a
  plain xor would miss. It catches corrupted or torn records. It is not a strong hash: two flipped bits can cancel out.

## How it is tested

- `make host-test`: 578 checks, including all 512 single-bit flips, a checksum value computed separately in Python,
  a canary slot past the end of the log, and `seq` wrapping from `0xFFFFFFFF` to 0.
- `make test`: four U54s run different workloads and hart 0 validates every record: checksum, hart, `seq` order, and
  that `mtime`, `mcycle` and `minstret` never go backwards. It also checks that `mepc` points into the right workload and
  that read and write misses follow the workload.
- Bugs were planted on purpose to prove the tests can fail. See the Day 2 log.

## What could go wrong

- **The ISR gets too slow.** Each tick now does a snapshot, a checksum and 64 bytes of stores. The cost is not measured
  yet. It is part of the Day 6 overhead sweep.
- **Reading a record while it is being written.** Hart 0 only reads after `done` is set, so that cannot happen today.
  Day 3's ring has to solve this properly with store-release and load-acquire.
- **The log fills up.** After 4096 samples everything is dropped and counted. Day 3's drain fixes this.
