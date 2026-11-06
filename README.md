# HartWatch-Multicore-RISC-V-Telemetry-Firmware
Because measuring a CPU shouldn't slow it down. A RISC-V monitor core (E51) does the boring parts so the application cores (U54) can keep working.


## The problem

RISC-V performance counters (`mcycle`, `minstret`, `mhpmcounter*`) are **hart-local**: a core can only read its own. So every sample is paid for by the core being measured, and measuring it changes what you measure.

## The idea

The E51 is a small monitor core that runs no application work. HartWatch keeps the cost on the application cores to a counter snapshot and moves everything else to the E51:

- **U54 harts:** a timer interrupt reads the counters and `mepc` and writes one 64-byte record into a private ring buffer.
- **E51 hart:** drains the rings, encodes and frames the data, and sends it out over a UART.
- **Host:** decodes the stream, with `mepc` to function name mapping planned.

Rings are single-producer/single-consumer with **drop-on-full**: the sampled core never blocks, and dropped samples are counted and reported rather than hidden.

## Architecture

```mermaid
flowchart LR
    subgraph SoC["QEMU sifive_u"]
        direction TB
        subgraph APP["Application harts 1-4 (U54)"]
            W["Workload"]
            ISR["Timer ISR<br/>read counters + mepc"]
            W -. "1 kHz interrupt" .-> ISR
        end
        RING[("Per-hart SPSC ring<br/>L2-LIM, 64 B slots<br/>drop-on-full")]
        subgraph MON["Monitor hart 0 (E51)"]
            DRAIN["Drain loop"]
            ENC["Delta + varint<br/>CRC + COBS frame"]
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
    DB --> API["REST + WebSocket API"]
    API --> DASH["Dashboard"]
    DEC --> GATE["CI regression gate"]

    classDef planned stroke-dasharray: 5 5;
    class SYM,DB,API,DASH,GATE planned;
```

Dashed boxes are **planned**; see [Status](#status).

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

Raw ring rate for 4 harts (calculated, not measured): 25.6 KB/s at 100 Hz, 256 KB/s at 1 kHz, 5.12 MB/s at 20 kHz.

## Status

| Day | Milestone | State |
| --- | --- | --- |
| 1 | Toolchain, multi-hart boot, UART, 1 kHz timer interrupts | done |
| 2 | Counter investigation, `hpm_backend`, sampling ISR, host unit tests | done |
| 3 | Lock-free rings, E51 drain, encoding and framing, host decoder | done, awaiting review |
| 4 | Collector, schema, symbolization | next |
| 5 | API, tests, dashboard, containers | planned |
| 6 | Benchmarks, overhead sweep, CI gate | planned |
| 7 | Design doc, demo, v1.0 | planned |

## What has been measured so far

All results are from QEMU 8.2.2. None of them describe real hardware. `-icount` means `-icount shift=0,sleep=off` (deterministic virtual time, 1 instruction = 1 ns). Raw output is in [`docs/data/`](docs/data/).

- **Timer ticks:** under `-icount`, gaps are 999-1001 us when few harts are busy. With busy harts, ticks arrive late (never early), by an amount that grows with the number of busy harts: worst lateness was 1, 41, 73 and 98 us with 1, 2, 3 and 4 busy harts at 1 kHz. QEMU runs harts round-robin, and a hart can only take its interrupt on its own turn. Workload and ISR length made no difference. The exact 98 us is not fully explained. Without `-icount`, tick spacing varied by milliseconds from run to run.
- **IPC is flat on QEMU:** 1.000 for every workload under `-icount`, 0.994-1.000 without. An IPC-based regression gate cannot work here, so the gate uses other metrics (below).
- **`minstret` is exact only in one setup:** under `-icount`, on one hart with the others asleep, it was within 16 instructions of the true count on 35-48 million. With other harts busy it reads global virtual time and is not per-hart.
- **The pipeline works end to end:** in `make test`, 8000 records from 4 harts pass through the rings, encoding and UART1 to the Python decoder, with 0 drops, in both timing modes. A digest of the raw records matches the decoded ones. A separate build overflows its rings, and every drop is accounted for.
- **Wire size:** 15.84 bytes/sample against 64 raw, and 63,384 bytes per virtual second for 4 harts at 1 kHz under `-icount`. See [`docs/DESIGN.md`](docs/DESIGN.md) for other rates. QEMU's UART has no baud limit. A real 115200 baud UART carries 11.52 KB/s (calculated).
- **Ring stress:** 75,000,915 records verified on RISC-V firmware under QEMU with 0 bad, and `seq` gaps equal to the drop counter. **This does not prove memory ordering.** With any ring barrier removed, this test still passed, while ThreadSanitizer on the host caught all four. See [`docs/ring.md`](docs/ring.md).
- **With sampling off, DTLB misses separate workloads cleanly:**

| Workload | DTLB read miss | DTLB write miss |
| --- | --- | --- |
| Arithmetic / branch-heavy | 0 | 0 |
| Load, stride 64 B | 78,125 | 0 |
| Load, stride 4 KiB + 64 B | 5,000,000 | 0 |
| Store, stride 64 B | 0 | 78,125 |
| Store, stride 4 KiB + 64 B | 0 | 5,000,000 |

With sampling on, the separation is no longer exact: the store hart showed 94,536 read misses under `-icount`, about 0.1% of its write misses. The ISR's own accesses adding these misses is **inferred, not isolated**: with no ISR the count was exactly 0, but no experiment has shown which access misses. The CI gate is measured with sampling disabled, and the extra misses are reported as overhead in the Day 6 sweep.

The ITLB-miss counter is unproven. It reached about 26,000 on the load and store harts with sampling on, which is not evidence of code footprint.

## QEMU vs. real silicon

The silicon column has not been measured. A fuller table is in [`docs/DESIGN.md`](docs/DESIGN.md).

| Counter | QEMU (measured) | FU540 silicon (not measured) |
| --- | --- | --- |
| `mcycle`, `minstret` | one clock, not per-hart under `-icount` | separate per-hart counters |
| DTLB / ITLB miss events | counted from QEMU's software TLB, not the U54's | real, different selector encoding |
| Branch mispredict, I-cache miss | SiFive-style selectors read 0 | real; needs SiFive raw event bits |

Counter access sits behind an `hpm_backend` interface (`hpm_qemu`, `hpm_sifive`). Real IPC lives in `hpm_sifive` and is **untested on hardware**.

## CI gate (design)

Because IPC is flat on QEMU, the gate fails a change when **instructions retired** (`minstret`) or **DTLB misses per thousand instructions** rise more than a configurable threshold (default 3%) against a stored baseline. Both are measured for a fixed amount of work on one U54, with the E51 asleep and sampling disabled. Under `-icount` results are deterministic, so a plain threshold is used. Mann-Whitney U is reserved for a separate wall-clock job, which measures QEMU host runtime and not target performance.

## Quick start

```bash
# Ubuntu 24.04 / WSL2
sudo apt install -y make git python3 gcc-riscv64-unknown-elf \
    binutils-riscv64-unknown-elf qemu-system-misc gdb-multiarch device-tree-compiler

scripts/check_env.sh   # verify the toolchain
make test              # host unit tests, then boot 5 harts in QEMU, expect PASS
```

On macOS, use the pinned toolchain container:

```bash
docker build -f Dockerfile.dev -t hartwatch-dev .
docker run --rm -v "$PWD":/work hartwatch-dev make test
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
fw/tests/        host unit tests (make host-test)
host/            decoder and stream checker (collector planned)
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
- [`docs/hpm_backend.md`](docs/hpm_backend.md), [`docs/sampler.md`](docs/sampler.md), [`docs/wire.md`](docs/wire.md), [`docs/drain.md`](docs/drain.md), [`docs/decoder.md`](docs/decoder.md), [`docs/counter_probe.md`](docs/counter_probe.md) -- plain-language module notes
