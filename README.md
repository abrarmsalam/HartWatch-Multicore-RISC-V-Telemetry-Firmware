# HartWatch-Multicore-RISC-V-Telemetry-Firmware
Because measuring a CPU shouldn't slow it down. A RISC-V monitor core (E51) does the boring parts so the application cores (U54) can keep working.


## The problem

RISC-V performance counters (`mcycle`, `minstret`, `mhpmcounter*`) are **hart-local**: a core can only read its own. So every sample is paid for by the core being measured, and measuring it changes what you measure.

## The idea

The E51 is a small monitor core that runs no application work. HartWatch keeps the cost on the application cores to a counter snapshot and moves everything else to the E51:

- **U54 harts:** a timer interrupt reads the counters and `mepc` and writes one 64-byte record into a private ring buffer.
- **E51 hart:** drains the rings, encodes and frames the data, and sends it out over a UART.
- **Host:** decodes the stream, maps `mepc` to function and line, stores it in TimescaleDB, and serves it through a REST API.

Rings are single-producer/single-consumer with **drop-on-full**: the sampled core never blocks, and dropped samples are counted and reported rather than hidden.

## Architecture

```mermaid
flowchart LR
    subgraph SoC["QEMU sifive_u"]
        direction TB
        subgraph APP["Application harts 1-4 (U54)"]
            W["Workload"]
            ISR["Timer ISR<br/>read counters + mepc"]
            W -. "timer interrupt" .-> ISR
        end
        RING[("Per-hart SPSC ring<br/>L2-LIM, 64 B slots<br/>drop-on-full")]
        subgraph MON["Monitor hart 0 (E51)"]
            DRAIN["Drain loop"]
            ENC["Delta + zigzag + varint<br/>CRC-32 + COBS frame"]
            DRAIN --> ENC
        end
        ISR -- "release store" --> RING
        RING -- "acquire load" --> DRAIN
        CLINT["CLINT<br/>shared mtime"]
        CLINT -. timestamps .-> ISR
    end

    ENC -- "UART1" --> DEC["Host decoder<br/>(Python)"]
    DEC --> SYM["ELF/DWARF<br/>symbolization"]
    SYM --> DB[("TimescaleDB")]
    DB --> API["REST API"]
    API --> DASH["Dashboard"]

    BENCH["Benchmark image<br/>sampling off, one U54"] --> GATE["CI gate"]
```

The CI gate is a separate path: it needs sampling disabled, so it does not read the telemetry stream.

## The sample record

One record is exactly one 64-byte cache line.

```c
typedef struct {
    uint32_t seq;        /*  0: per-hart sequence number             */
    uint8_t  hart;       /*  4                                       */
    uint8_t  flags;      /*  5                                       */
    uint16_t check;      /*  6: test instrumentation (SAMPLE_CHECKSUM) */
    uint64_t mtime;      /*  8: shared timeline, read in the ISR     */
    uint64_t mcycle;     /* 16                                       */
    uint64_t minstret;   /* 24                                       */
    uint64_t hpm[3];     /* 32: DTLB read, DTLB write, ITLB miss     */
    uint64_t mepc;       /* 56: interrupted instruction              */
} sample_t;              /* 64 bytes                                 */
```

Raw ring rate for 4 harts (calculated): 25.6 KB/s at 100 Hz, 256 KB/s at 1 kHz, 5.12 MB/s at 20 kHz. Encoded rates are measured, below.

## Status

| Day | Milestone | State |
| --- | --- | --- |
| 1 | Toolchain, multi-hart boot, UART, 1 kHz timer interrupts | done |
| 2 | Counter investigation, `hpm_backend`, sampling ISR, host unit tests | done |
| 3 | Lock-free rings, E51 drain, encoding and framing, host decoder | done |
| 4 | Collector, schema, symbolization | done |
| 5 | API, tests, dashboard, containers | done, minimal: no WebSocket, dashboard not viewed in a browser |
| 6 | Benchmarks, overhead sweep, CI gate | done |
| 7 | Design doc, README, demo | done. No v1.0 tag: left to the owner |

## What has been measured so far

All results are from QEMU 8.2.2. None of them describe real hardware. `-icount` means `-icount shift=0,sleep=off` (deterministic virtual time, 1 instruction = 1 ns). Raw output is in [`docs/data/`](docs/data/).

- **Timer ticks:** under `-icount`, gaps are 999-1001 us when few harts are busy. With busy harts, ticks arrive late (never early), by an amount that grows with the number of busy harts: worst lateness was 1, 41, 73 and 98 us with 1, 2, 3 and 4 busy harts at 1 kHz. QEMU runs harts round-robin, and a hart can only take its interrupt on its own turn. Workload and ISR length made no difference. The exact 98 us is not fully explained. Without `-icount`, tick spacing varied by milliseconds from run to run.
- **IPC is flat on QEMU:** 1.000 for every workload under `-icount`, 0.994-1.000 without. An IPC-based regression gate cannot work here, so the gate uses other metrics (below).
- **`minstret` is exact only in one setup:** under `-icount`, on one hart with the others asleep, it was within 16 instructions of the true count on 35-48 million. With other harts busy it reads global virtual time and is not per-hart.
- **The pipeline works end to end:** in `make test`, 8000 records from 4 harts pass through the rings, encoding and UART1 to the Python decoder, with 0 drops, in both timing modes. A digest of the raw records matches the decoded ones. A separate build overflows its rings, and every drop is accounted for.
- **Wire size:** 15.84 bytes/sample against 64 raw, and 63,384 bytes per virtual second for 4 harts at 1 kHz under `-icount`. See [`docs/DESIGN.md`](docs/DESIGN.md) for other rates. QEMU's UART has no baud limit. A real 115200 baud UART carries 11.52 KB/s (calculated).
- **Ring stress:** 75,000,915 records verified on RISC-V firmware under QEMU with 0 bad, and `seq` gaps equal to the drop counter. **This does not prove memory ordering.** With any ring barrier removed, this test still passed, while ThreadSanitizer on the host caught all four. The ring's ordering is argued and TSan-mutation-tested, not proven on RISC-V. See [`docs/ring.md`](docs/ring.md).
- **ISR cost, under `-icount`:** 194 instructions per tick with `SAMPLE_CHECKSUM=0` (112 of those are trap and timer handling), 250 with it on.
- **Overhead on the workload hart, under `-icount`:** 0.0194% of retired instructions at 1 kHz and 0.3895% at 20 kHz. It crosses 1% between 50,000 Hz (0.9795%) and 52,631.6 Hz (1.0315%). That is a share of retired instructions on QEMU, **not a time overhead on real hardware**.
- **E51 limit, under `-icount`:** with 4 harts ticking, no drops up to 20 kHz, 16.99-19.20% dropped at 50 kHz, 53.59-58.41% at 100 kHz. Drain costs 861.7-969.1 instructions per sample.
- **CI gate:** fails on a deliberate slowdown (+12.500% `minstret`) and on a stride change with the same instruction count (+100% DTLB misses), and passes on clean code. See [`docs/benchmarks.md`](docs/benchmarks.md).
- **Hot functions from a real run:** each hart's workload came first, 99.70-99.95% of samples. These are sampling estimates.
- **GitHub Actions:** build, tests and gate have passed on GitHub's `ubuntu-24.04` runner (`x86_64`), with the same 126,723-byte stream size and gate result as the local arm64 run.
- **With sampling off, DTLB misses separate workloads cleanly:**

| Workload | DTLB read miss | DTLB write miss |
| --- | --- | --- |
| Arithmetic / branch-heavy | 0 | 0 |
| Load, stride 64 B | 78,125 | 0 |
| Load, stride 4 KiB + 64 B | 5,000,000 | 0 |
| Store, stride 64 B | 0 | 78,125 |
| Store, stride 4 KiB + 64 B | 0 | 5,000,000 |

With sampling on, the separation is no longer exact: the store hart showed 94,536 read misses under `-icount`, about 0.1% of its write misses. The ISR's own accesses adding these misses is **inferred, not isolated**: with no ISR the count was exactly 0, and on large-stride workloads the extra misses scale with ticks, but no experiment has shown which access misses. The CI gate is measured with sampling disabled, and the extra misses are reported as overhead in the Day 6 sweep.

The ITLB-miss counter is unproven. It reached about 26,000 on the load and store harts with sampling on, which is not evidence of code footprint.

## QEMU vs. real silicon

The silicon column has not been measured. A fuller table is in [`docs/DESIGN.md`](docs/DESIGN.md).

| Counter | QEMU (measured) | FU540 silicon (not measured) |
| --- | --- | --- |
| `mcycle`, `minstret` | one clock, not per-hart under `-icount` | separate per-hart counters |
| DTLB / ITLB miss events | counted from QEMU's software TLB, not the U54's | real, different selector encoding |
| Branch mispredict, I-cache miss | SiFive-style selectors read 0 | real; needs SiFive raw event bits |

Counter access sits behind an `hpm_backend` interface (`hpm_qemu`, `hpm_sifive`). Real IPC lives in `hpm_sifive` and is **untested on hardware**.

## CI gate

Because IPC is flat on QEMU, the gate fails a change when **instructions retired** (`minstret`) or **DTLB misses per thousand instructions** rise more than a configurable threshold (default 3%) against a stored baseline. Both are measured for a fixed amount of work on one U54, with the E51 asleep and sampling disabled. Under `-icount` results are deterministic, so a plain threshold is used. Mann-Whitney U is reserved for a separate wall-clock job, which measures QEMU host runtime and not target performance.

## Quick start

```bash
# Ubuntu 24.04 / WSL2
sudo apt install -y make git python3 gcc-riscv64-unknown-elf \
    binutils-riscv64-unknown-elf qemu-system-misc gdb-multiarch device-tree-compiler

scripts/check_env.sh   # verify the toolchain
make test              # host tests, boot 5 harts in QEMU, decode the telemetry, expect PASS
make gate              # compare fixed-work counters with bench/baseline.json
```

On macOS, use the pinned toolchain container:

```bash
docker build -f Dockerfile.dev -t hartwatch-dev .
docker run --rm -v "$PWD":/work hartwatch-dev make test
```

The whole pipeline, from boot to hot functions in the database, needs Docker:

```bash
scripts/demo.sh
docker compose up -d api    # REST API and dashboard on http://127.0.0.1:8000
```

Other commands: `make run`, `make debug` + `make gdb`, `make disasm`, `make dtb`, `make test-drops`, `make stress`, `make probe`, `make tickgap`. See [SETUP.md](SETUP.md) for details.

**Debugging note:** GDB controls hart 0 only with this QEMU; inspect the U54s through the QEMU monitor (`cpu N`, `info registers`).

## Repository layout

```
fw/boot.S        first instructions every hart runs; trap entry
fw/linker.ld     memory layout
fw/src/          C firmware and workloads
fw/src/hpm/      counter backends, chosen at build time
fw/probe/        experiment images (make probe, make tickgap, make stress)
fw/bench/        benchmark image for the gate and the overhead sweep
bench/           the gate's stored baseline
fw/tests/        host unit tests (make host-test)
host/            decoder, symbolizer, collector, API, gate, dashboard
scripts/         environment check, test runner, experiment scripts
docs/            walkthroughs, module notes, design, engineering log
docs/data/       raw output behind the measured numbers
Dockerfile.dev   pinned toolchain for non-Ubuntu hosts
```

## Non-goals

No custom RTOS, no Linux/OpenSBI port, no hand-rolled storage engine, no authentication, no RTL.

## Documentation

- [`docs/DAY1.md`](docs/DAY1.md) -- bring-up explained from zero
- [`docs/ENGINEERING_LOG.md`](docs/ENGINEERING_LOG.md) -- what was observed, tested and decided
- [`docs/DESIGN.md`](docs/DESIGN.md) -- record format, CI gate, QEMU-versus-silicon table, ring ordering argument
- [`docs/ring.md`](docs/ring.md) -- ring invariants, the ordering argument, and what the tests can and cannot show
- [`docs/hpm_backend.md`](docs/hpm_backend.md), [`docs/sampler.md`](docs/sampler.md), [`docs/wire.md`](docs/wire.md), [`docs/drain.md`](docs/drain.md), [`docs/decoder.md`](docs/decoder.md), [`docs/symbols.md`](docs/symbols.md), [`docs/collector.md`](docs/collector.md), [`docs/api.md`](docs/api.md), [`docs/counter_probe.md`](docs/counter_probe.md) -- plain-language module notes
- [`docs/benchmarks.md`](docs/benchmarks.md) -- CI gate, overhead sweep, ISR and drain cost
