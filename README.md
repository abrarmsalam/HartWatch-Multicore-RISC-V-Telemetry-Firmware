# HartWatch

Low-overhead CPU telemetry on a RISC-V monitor core. Application harts (U54) pay only for a counter snapshot;
the monitor hart (E51) aggregates, encodes and ships the data. Target: QEMU `sifive_u`.

## Status

| Day | Milestone | State |
| --- | --- | --- |
| 1 | Toolchain, repo, multi-hart boot, UART, 1 kHz timer interrupts | **done** |
| 2 | HPM counter abstraction, sampling ISR, host-compiled unit tests | **done, awaiting review** |
| 3 | Lock-free rings, E51 drain loop, encoding and framing, host decoder | next |
| 4 | Collector, schema, symbolization | |
| 5 | API, tests, minimal dashboard, containerized stack | |
| 6 | Benchmark suite, overhead sweep, CI regression gate | |
| 7 | Design doc, README, demo, v1.0 | |

## Start here

1. [`SETUP.md`](SETUP.md) -- install the tools and run `make test`.
2. [`docs/DAY1.md`](docs/DAY1.md) -- what Day 1 builds, explained from zero, with exercises.
3. [`docs/ENGINEERING_LOG.md`](docs/ENGINEERING_LOG.md) -- what was observed, tested and decided.
4. [`docs/DESIGN.md`](docs/DESIGN.md) -- record format, CI gate, and the QEMU-versus-silicon table.

## Measured so far

All on QEMU 8.2.2. None of this describes real hardware.

- IPC does not vary on QEMU: 1.000 for every workload under `-icount`, 0.994-1.000 without it. The CI gate therefore
  uses `minstret` for fixed work, not IPC. Raw data is in `docs/data/`.
- Under `-icount`, on one hart with the others asleep, `minstret` was within 16 instructions of the true count on runs
  of 35-48 million instructions.
- `make test`: 8000 records from 4 harts validated with 0 drops, in both timing modes.

## Layout

```
fw/boot.S          first instructions every hart runs; trap entry
fw/linker.ld       memory layout
fw/src/            C firmware (uart, kprintf, timer, trap, sampler, main) and workloads
fw/src/hpm/        counter backends, chosen at build time
fw/probe/          counter probe firmware (make probe)
fw/tests/          host unit tests (make host-test)
scripts/           check_env.sh, run_test.sh
docs/              walkthroughs, design, engineering log, raw results (docs/data/)
host/              (empty until Day 3: decoder and collector)
Makefile           build / run / test / debug
```
