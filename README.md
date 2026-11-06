# HartWatch

Low-overhead CPU telemetry on a RISC-V monitor core. Application harts (U54) pay only for a counter snapshot;
the monitor hart (E51) aggregates, encodes and ships the data. Target: QEMU `sifive_u`.

## Status

| Day | Milestone | State |
| --- | --- | --- |
| 1 | Toolchain, repo, multi-hart boot, UART, 1 kHz timer interrupts | **done** |
| 2 | HPM counter abstraction, sampling ISR, host-compiled unit tests | next |
| 3 | Lock-free rings, E51 drain loop, encoding and framing, host decoder | |
| 4 | Collector, schema, symbolization | |
| 5 | API, tests, minimal dashboard, containerized stack | |
| 6 | Benchmark suite, overhead sweep, CI regression gate | |
| 7 | Design doc, README, demo, v1.0 | |

## Start here

1. [`SETUP.md`](SETUP.md) -- install the tools and run `make test`.
2. [`docs/DAY1.md`](docs/DAY1.md) -- what Day 1 builds, explained from zero, with exercises.
3. [`docs/ENGINEERING_LOG.md`](docs/ENGINEERING_LOG.md) -- what was observed, tested and decided.

## Layout

```
fw/boot.S          first instructions every hart runs; trap entry
fw/linker.ld       memory layout
fw/src/            C firmware (uart, kprintf, timer, trap, main)
scripts/           check_env.sh, run_test.sh
docs/              walkthroughs and the engineering log
host/              (empty until Day 3: decoder and collector)
Makefile           build / run / test / debug
```
