# CLAUDE.md -- HartWatch

## What this project is

HartWatch is a low-overhead CPU telemetry system for a multi-core RISC-V target (QEMU `sifive_u`: 1x E51 monitor hart + 4x U54 application harts).
Performance counters are hart-local, so sampling cost lands on the core being measured. The design keeps the application harts' cost to a counter
snapshot and moves aggregation, encoding and I/O to the E51.

Pipeline: sample (U54 timer ISR) -> SPSC ring in L2-LIM -> E51 drain/encode/frame -> UART1 -> host decoder -> TimescaleDB -> REST/WebSocket API -> CI regression gate.

The owner is a university student who will be asked about every part of this in interviews. Code must be understandable, and every module must be explainable.

## Current state

Day 1 is done and lives in this repo: multi-hart boot (`fw/boot.S`), linker script, UART, `kprintf`, 1 kHz timer interrupts on harts 1-4, `make test`.
Start from Day 2. Do not rewrite Day 1 unless something is broken; if so, say why.

## Environment

Ubuntu 24.04, QEMU 8.2.2, `riscv64-unknown-elf-gcc` 13.2, `gdb-multiarch`, Python 3. `make test` must pass before and after every change.
QEMU runs with `-icount shift=0,sleep=off` by default. Default QEMU timing is noisy (ms-scale jitter); always say which mode a number was measured in.

Known limits found on Day 1:
- GDB attaches to hart 0 only. To inspect U54s use the QEMU monitor: `cpu N`, `info registers`.
- Under icount, time is virtual (1 instruction = 1 ns), so cycles may just equal instructions.

## Day 2 findings (measured)

- IPC is 1.000 for every workload under `-icount` and 0.994-1.000 without it. An IPC gate can never fire on QEMU.
- `minstret` under `-icount` is within 16 instructions of the true count on 35-48 million instructions.
- With sampling OFF, DTLB read and write miss counts separate loads from stores with no crossover, and are identical with and without `-icount`.
  Stride 64 B: 78,125 misses; stride 4 KiB+64 B: 5,000,000 misses; arithmetic and branch-heavy: 0.
- With sampling ON, the crossover is no longer zero: the ISR's own accesses add DTLB misses (inferred, not isolated; e.g. 94,536 read misses on the store hart, about 0.1% of its write misses, under `-icount`). This extra is an overhead signal for the Day 6 sweep, not a workload metric.
- ITLB miss is unproven. It reached about 26,000 on the load and store harts with sampling on, but that is not evidence of code footprint: none of these workloads has a large code footprint.
- Tick lateness under `-icount` comes from QEMU's round-robin scheduling, not from the host, the workload or the ISR. See the Day 2 log.

## Hard rules (read these first)

1. **Never fabricate or estimate a result.** Every number in docs, logs, READMEs and commit messages must come from a run you actually did. If you have not measured it, write "not measured".
2. **Verify before building on an assumption.** Day 2's first task is to find out what `mcycle`, `minstret` and `mhpmcounterN` actually report under QEMU, with and without `-icount`, on a workload with different behaviors. Report the findings. If IPC does not vary between workloads, STOP and ask the owner how to proceed. Do not build the IPC gate on a metric that does not move.
3. **Stop at the end of each day** with a short report (what works, test output, what was measured, open problems) and wait for review before starting the next day.
4. **No claims beyond evidence.** Do not describe QEMU results as hardware behavior. Keep a QEMU-vs-silicon table in the docs and update it as you learn.
5. **Evidence for correctness, not just "it ran".** Ring buffer correctness needs a stress test with sequence numbers and checksums over millions of samples, plus host-compiled ring tests run under ThreadSanitizer where possible. GDB step-through is NOT evidence of memory-ordering correctness.
6. **Explain as you go.** For every new module, write a short plain-language note in `docs/` (what it does, why this design, what could go wrong). Assume the reader is a second-year engineering student.
7. **Small, tested steps.** Build, run, and show output for each step. Commit per logical step. Do not leave the repo in a broken state.

## Design decisions (fixed unless the owner changes them)

- Samples: each U54 timer ISR reads `mcycle`, `minstret`, 3 `mhpmcounter`s and `mepc`.
- **Record layout (decided): 64-byte `sample_t`, `aligned(64)`, one cache line per slot.** Fields: `uint32_t seq`, `uint8_t hart`, `uint8_t flags`, `uint16_t check` (test instrumentation behind `SAMPLE_CHECKSUM`: on for tests, off for the overhead sweep; not part of the telemetry design), `uint64_t mtime` (read in the ISR), `uint64_t mcycle`, `uint64_t minstret`, `uint64_t hpm[3]` (DTLB read miss, DTLB write miss, ITLB miss), `uint64_t mepc`. The proposal's "32 bytes" did not fit six 64-bit values. Raw ring rate for 4 harts: 25.6 KB/s at 100 Hz, 256 KB/s at 1 kHz, 5.12 MB/s at 20 kHz (calculated, not measured). Ring memory 4096 slots x 4 harts = 1 MiB.
- Rings: one single-producer/single-consumer ring per U54 in L2-LIM (`0x08000000`), power-of-two size, `head` and `tail` on separate 64-byte cache lines, store-release / load-acquire, **drop-on-full with a drop counter**. The producer never blocks and never prints.
- Timer: next deadline = previous deadline + period (no drift). ISRs must never call `kprintf`.
- E51 drain: poll 4 rings, delta + zigzag + varint encode, CRC, COBS framing, `0x00` delimiter, UART1. The timestamp is `mtime` recorded in the sample ISR (sample time, not drain time).
- Counter access behind `hpm_backend` with `hpm_qemu` and `hpm_sifive` implementations selected at build time.
- Host: Python 3. Decoder first (pure functions, unit-tested with pytest), then collector, then TimescaleDB schema, then DWARF/ELF symbolization (`pyelftools`).
- CI gate (QEMU, `-icount`): IPC is flat on QEMU (see Day 2 findings), so there is NO IPC gate. The primary gate metric is `minstret` for a fixed amount of work on one U54 with the E51 asleep and sampling DISABLED (a clean workload metric), plus DTLB read/write misses per thousand instructions, also with sampling disabled. Variance is zero under icount, so use a plain threshold (default 3%, configurable) against a stored baseline; Mann-Whitney U is not used here. Prove the gate works by injecting a deliberate slowdown and showing it fails.
- Mann-Whitney U is used only in a separate non-icount wall-clock job, labelled as measuring QEMU host runtime, not target performance.
- Real IPC and the SiFive-specific selectors stay in `hpm_sifive`, documented as untested on hardware.
- Workloads: arithmetic, branch-heavy, load stride 64 B, load stride 4 KiB+64 B, store stride 64 B, store stride 4 KiB+64 B. Do not call these "cache-thrashing": QEMU does not model caches, and the miss counts are QEMU's software TLB, not the FU540's.
- Overhead sweep: same workload with telemetry off, then 100 Hz to 20 kHz, built with `SAMPLE_CHECKSUM=0`. Report workload runtime delta, ring occupancy, drop rate, the extra DTLB misses caused by sampling, and the rate at which runtime increases by more than 1%.

## Non-goals

No RTOS, no Linux/OpenSBI port, no custom storage engine, no authentication, no RTL.

## Code style

- C: `-Wall -Wextra` clean, no dynamic allocation, no libc, comments explain *why*. Shared constants in `fw/src/config.h`.
- Python: type hints, pytest, small pure functions.
- Keep the `Makefile` as the single entry point (`make`, `make test`, `make run`, `make debug`).

## Documentation to maintain

- `docs/ENGINEERING_LOG.md`: one entry per day: observed / hypothesis / test / result / decision. Record defects, root causes and surprises, including failures.
- `docs/DESIGN.md`: architecture, wire format spec, porting notes (FU540 silicon selectors, Arm PMU mapping).
- `README.md`: status table, how to run, measured results only.

## Definition of done per day

- Day 2: counter investigation reported; record layout decided; `hpm_backend`; sampling ISR writing records; host-compiled unit tests passing.
- Day 3: rings + stress test passing; E51 drain, encoding, framing; host decoder round-trips real firmware output; drops counted and reported.
- Day 4: collector, schema, ELF/DWARF symbolization; a SQL query returns hot functions per hart.
- Day 5: REST/WebSocket API with OpenAPI docs and pytest; `docker compose up`; minimal dashboard.
- Day 6: benchmark workloads, overhead sweep with results, minstret/DTLB CI gate with a demonstrated failing case.
- Day 7: design doc, README with only measured numbers, demo script, tag v1.0.