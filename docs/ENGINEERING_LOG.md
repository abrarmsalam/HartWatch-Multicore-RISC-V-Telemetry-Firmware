# Engineering log

One entry per day: what was observed, what was hypothesised, what was tested, what was decided.
Only record things that actually happened. A hypothesis is marked as one until a test confirms it.

---

## Day 1

**Environment.** Ubuntu 24.04, QEMU 8.2.2, riscv64-unknown-elf-gcc 13.2.0, GDB 15.1. Build host had 2 CPU cores.
`-M sifive_u -smp 5` gives hart 0 = `rv64acimu` (E51) and harts 1-4 = `rv64acdfimsu` (U54), read from each hart's own
`misa` CSR. Device addresses and the 1 MHz timebase were taken from QEMU's dumped device tree, not assumed.

### Finding 1 -- tick timing in default QEMU mode is noisy

- **Observed.** Each U54 taking 2000 ticks at a requested 1 kHz. Mean gap was ~998-1000 us in every mode, but spread varied:
  - default (multi-threaded TCG), first run: max gap 15.9-20.1 ms, 95-103 of 2000 ticks "late" (gap > 2x period)
  - default, later run: max gap ~8.5 ms, 3-6 late
  - single-threaded TCG: max gap 4.2-5.1 ms, 4-5 late
  - minimum gap was 5-8 us in all of these (late ticks are followed by catch-up ticks)
- **Hypothesis (not proven).** With 2 host cores simulating 5 harts, QEMU's timer, which follows the host clock, is delayed
  whenever host threads are descheduled.
- **Test.** `-icount shift=0,sleep=off` (virtual time, 1 instruction = 1 ns).
- **Result.** All 8000 gaps in 999-1001 us, 0 late ticks, identical output on 3 consecutive runs. Wall time 0.8 s vs 2.1 s.
- **Decision.** `-icount shift=0,sleep=off` is the default in the `Makefile` for run, test and debug. Any later claim about
  timing or overhead must say it was measured in this mode.
- **Open question for Day 2.** Under icount, virtual time derives from the instruction count, so `mcycle` may simply track
  `minstret` and IPC may be ~constant. Not yet checked. This must be verified before building the IPC regression gate on it.

### Finding 2 -- GDB controls only hart 0

- **Observed.** After `target remote :1234`, `info threads` lists one thread, `Thread 1.1 (sifive-e51-riscv-cpu harts[0])`.
  A breakpoint on `hart_main` (code that only harts 1-4 run) never fires: hart 0 runs to completion and the session ends.
- **Tried.** `target extended-remote` plus `attach 2`: QEMU dropped the connection (broken pipe). A variant with
  `add-inferior` / `inferior 2` hung until killed.
- **Interpretation (unconfirmed).** The `1.1` thread-ID format suggests QEMU's gdbstub exposes the E51 and the U54s as
  separate processes; GDB attaches only to the first.
- **Status.** Not root-caused. Workaround: the QEMU monitor. `info cpus` lists all 5 harts and `cpu 2` followed by
  `info registers` shows hart 2's registers (confirmed: `mhartid 0000000000000002`). Verified only on a paused machine,
  not yet while U54 code is running. GDB remains fine for hart 0 and for host-compiled unit tests.

### Design decisions

- Build for `rv64imac` so one binary runs on both core types (U54s are a superset).
- Per-hart stacks live in their own section, outside `.bss`, because hart 0 zeroes `.bss` while running on its own stack.
- `trap_entry` saves only the 16 caller-saved registers; the C ABI preserves the rest. This is the sampling-ISR path.
- Timer deadlines advance by `previous_deadline + period`, so interrupt latency does not accumulate as drift.
- No `printf` in interrupt context (the print lock could deadlock against the interrupted code).

### Defects

None in the firmware this day beyond the toolchain notes above (one linker warning about an RWX segment, silenced with
`--no-warn-rwx-segments`).

---

## Day 2

**Environment.** Ubuntu 24.04 container (`Dockerfile.dev`) on an arm64 Mac: QEMU 8.2.2, riscv64-unknown-elf-gcc 13.2.0,
GDB 15.1. Homebrew was not used because it offers QEMU 11.1.3, not the pinned version. This differs from Day 1's host,
so no-icount timings are not comparable with Day 1's. Raw output of every number below is in `docs/data/probe_*.txt`
(3 runs per mode, 5 repetitions per workload per run).

### Finding 1 -- IPC does not vary on QEMU, so it cannot be the QEMU gate metric

- **Observed.** Hart 1 alone, `make probe`, workloads with known instruction counts:

  | Workload | True instructions | icount `minstret` error | icount IPC | no-icount IPC |
  | --- | --- | --- | --- | --- |
  | alu | 40,000,000 | +10 | 1.00000 | 0.99440-0.99996 |
  | branch | 48,000,000 | +11 | 1.00000 | 0.99978-0.99999 |
  | load, stride 64 B | 35,000,000 | +15 | 1.00000 | 0.99964-0.99997 |
  | load, stride 4 KiB + 64 B | 35,000,000 | +16 | 1.00000 | 0.99997-0.99999 |
  | store, stride 64 B | 35,000,000 | +15 | 1.00000 | 0.99976-0.99997 |
  | store, stride 4 KiB + 64 B | 35,000,000 | +16 | 1.00000 | 0.99993-1.00000 |

- **Cause (from the QEMU 8.2.2 source).** `get_ticks()` in `target/riscv/csr.c` returns `icount_get()` under icount and
  `cpu_get_host_ticks()` otherwise. It has no separate notion of instructions, so `mcycle` and `minstret` are one clock.
- **Supporting tests.**
  - `minstret` rose by 10,000,001 across a 10 ms `wfi` sleep under icount, so it does not count retired instructions.
  - With harts 2-4 busy under icount, hart 1's alu run read 208,000,004-292,000,001 instead of 40,000,010. The counters
    are not hart-local there: they follow global virtual time. The size of the factor (5-7x, not 4x) is not explained.
  - Without icount, `mcycle` tracks `mtime` x 1000 and `minstret` is wrong by up to about 1.7 billion. That is host
    time and says nothing about RISC-V behaviour.
  - The 3 icount runs were line-for-line identical, including the busy-hart phase.
- **Decision (owner, Option A).**
  - Primary gate metric: `minstret` for a fixed amount of work on one U54 with the E51 asleep, under icount. This is
    valid only in that setup, where it equals the true count to within 16 instructions.
  - DTLB read and write misses per thousand instructions are also gated.
  - Plain threshold (default 3%), no Mann-Whitney U in the icount gate: repeat runs are identical, so variance is zero
    and a rank test has nothing to work with.
  - Mann-Whitney U is kept only for a separate no-icount wall-clock job, labelled as measuring QEMU host runtime.
  - Real IPC stays in `hpm_sifive`, documented as untested on hardware.
  - No TCG plugin and no real-hardware path.

### Finding 2 -- QEMU's TLB events do move, and they are per-hart

- **Observed.** Per run, identical in both modes and with other harts busy:

  | Workload | DTLB read miss | DTLB write miss |
  | --- | --- | --- |
  | alu, branch | 0 | 0 |
  | load, stride 64 B | 78,125 | 0 |
  | load, stride 4 KiB + 64 B | 5,000,000 | 0 |
  | store, stride 64 B | 0 | 78,125 |
  | store, stride 4 KiB + 64 B | 0 | 5,000,000 |

  78,125 is 5,000,000 x 64 / 4096, the number of 4 KiB page crossings.
- **Limit.** These count fills of QEMU's software TLB. They are not hardware TLB misses.
- **ITLB miss did not separate these workloads.** One read per workload: 0-23 under icount, 0-1 without. None of these
  workloads has a large code footprint.
- **Decision (owner).** Three `mhpmcounter`s per sample: DTLB read miss, DTLB write miss, ITLB miss.

### Finding 3 -- PMU limits on this machine

- `mcountinhibit` traps as an illegal instruction on all harts tested (0 and 1): the source sets `sifive_u` CPUs to
  privilege spec 1.10 and the CSR needs 1.11. Counters cannot be paused.
- `mhpmcounter3`-`18` exist; `19`-`31` trap, although `mhpmevent3`-`31` are all readable.
- SiFive-style selector values read 0 on every workload. Those values were written from memory and are unverified, so
  this says nothing about silicon. In QEMU 8.2.2, `riscv_pmu_update_event_map` rejects unknown events.
- An old selector keeps counting after a new one is written, until 0 is written (`riscv_pmu_update_event_map` only
  removes a mapping for value 0). `hpm_qemu` must write 0 before changing a selector.

### Finding 4 -- sampling works, and it changes what the counters show

- **Decision (owner).** 64-byte `sample_t`, one cache line per slot, `mtime` recorded in the ISR. Layout and reasons are
  in `docs/DESIGN.md`.
- **Observed.** `make test`: each U54 runs its own workload through 2000 ticks at 1 kHz. All 8000 records passed
  validation in 3 `-icount` runs and 3 no-icount runs, with 0 drops. The 3 `-icount` runs were identical. Records were
  written to and read back from L2-LIM. Wall time for one `make test` under `-icount` was 18.7 s, against a 30 s timeout.

  | Hart | Workload | `mepc` in workload | DTLB read | DTLB write | ITLB |
  | --- | --- | --- | --- | --- | --- |
  | 1 | alu | 1997 / 2000 | 64 | 96 | 0 |
  | 2 | branch | 1999 / 2000 | 0 | 32 | 0 |
  | 3 | load | 1997 / 2000 | 102,520,026 | 5,997 | 26,147 |
  | 4 | store | 1996 / 2000 | 94,536 | 91,048,484 | 26,122 |

  (`-icount`, first to last sample. Without icount, `mepc` in workload was 1884-1998 of 2000.)
- **Surprise 1: tick gaps widened.** Under `-icount`, gaps were 999-1001 us with idle U54s on Day 1 and in the baseline
  today. With busy U54s they are 873-1098 us. Mean is still 999-1000 us and 0 ticks are late. Explained in Finding 5.
- **Surprise 2: read and write misses now cross over slightly.** The probe, with no ISR running, showed exactly 0 on
  the other event. With sampling on, the store hart shows 94,536 read misses, about 0.1% of its write misses.
  **Hypothesis (not tested):** the ISR's own memory accesses miss because the workload evicted their TLB entries.
- **Surprise 3: ITLB miss moved for the first time.** About 26,000 on the load and store harts, 0 on alu and branch.
  The probe's reading was 0-23. **Hypothesis (not tested):** the same eviction, hitting the code's TLB entries. So the
  ITLB slot responds to something, but not yet to code footprint, which is what it would normally be for.
- **No-icount reminder.** Gaps ranged from 1 us to 126,741 us and 18-113 ticks per hart were late. Miss counts were lower
  than under `-icount` and varied between runs. The time-based test limits passed, but these runs say nothing about
  timing quality.

### Finding 5 -- tick lateness comes from QEMU's round-robin scheduling (review item)

- **Observed.** `make tickgap` reproduced the widening. Ticks are never early, only late, so gaps below 1000 us are
  catch-up after a late tick. Raw data: `docs/data/tickgap_matrix.txt`. All `-icount`, 2000 ticks per hart.

  | Factor changed | Result (worst lateness on h1/h2) |
  | --- | --- |
  | Busy harts 0, 1 (h1), 2, 3, 4 | 1, 1, 41, 73, 98 us |
  | Workload: branch, load, store, mixed | 98 us every time |
  | ISR: no sampling, sampling, +1000 pad, +10000 pad | 98 us every time |
  | E51: 100 Hz tick or asleep | 98 us both (h1 72 us with E51 asleep) |
  | Period 500, 1000, 2000, 4000 us | 49, 98, 196, 391 us, about 9.8% of period |

- **Hypothesis.** In `accel/tcg/tcg-accel-ops-rr.c`, under icount all harts share one thread and take turns. Each slice
  is `time_to_next_deadline / cpu_count`, where `cpu_count` counts all CPUs. Timers are noticed between slices, and a
  hart can only take its interrupt on its own turn. So a tick waits for the other busy harts' slices.
- **Test.** That predicts lateness of `period / cpu_count` for 2 busy, ticking harts, growing as CPUs are removed.
  Measured 334, 250, 201 us at `-smp` 3, 4, 5. A small model of that rule gave 333, 250, 200 us.
- **Result.** Host scheduling, workload and ISR length are ruled out. Lateness is deterministic and comes from the
  scheduler rule. The dependence on hart position (h3 stays at 0-1 us in most runs) fits a rotation order effect.
- **Not explained.** The toy model does NOT reproduce the case with all 4 harts ticking: it gave up to 596 us against
  98 us measured, and it got which harts are late wrong. Other harts' deadlines shorten the slices in a way the model
  misses. The mechanism is supported, but the exact 98 us is unexplained. Hart 4's single 128 us outlier is also
  unexplained.
- **Also found.** With busy harts that have no timer, one ticking hart was up to 48,800 us late, since nothing shortens
  the slices. Any later experiment that mixes ticking and non-ticking busy harts must expect this.
- **Consequence.** Sample `mtime` jitter under `-icount` is a QEMU artifact and scales with the period. It says nothing
  about interrupt latency on silicon.

### Review fixes

- `SAMPLE_CHECKSUM` build flag added. The checksum is test instrumentation, not part of the telemetry design.
- CI gate is measured with sampling disabled, for the reasons in `docs/DESIGN.md`.
- Attribution of the extra misses to the ISR's own accesses is inferred: the probe, with no ISR, read exactly 0 on the
  other event. No experiment has isolated which ISR access misses.
- `make test` timeout is now `TEST_TIMEOUT`, default 120 s (was 30 s).

### Do the tests catch bugs?

Bugs were planted on purpose in a throwaway copy:

| Planted bug | Caught by |
| --- | --- |
| Checksum without the rotate | Host test |
| Drop-on-full removed | Host test (UBSan) |
| `seq` not advanced on a drop | Host test |
| `seq` skips a value | Firmware self-test |
| `mepc` lost | Host test and firmware self-test |
| Read and write selectors swapped | Firmware self-test, **only after a fix** |

### Defects

- Self-test hole: swapped read/write selectors passed `make test`, because the check compared the load hart with the alu
  hart. Fixed by comparing each hart's own read and write misses. Found only by planting the bug.
- First attempt at planting the `mepc` bug failed for an unrelated reason (a compile error from an unused parameter), so
  it proved nothing and was redone.
- First probe run: `%-9s` printed literally because `kprintf` has no `-` flag, shifting columns. Fixed by using `%9s`.
- First probe run: reading `mcountinhibit` trapped and left garbage in the result (`0x5 reads back 0xa`). Fixed by
  reporting traps per CSR.
- First probe run: selectors reused one counter and contaminated later rows (see Finding 3). Fixed with one counter each.
- `make clean` deleted probe logs kept in `build/`. Runs were repeated and raw output now lives in `docs/data/`.

---

## Day 3

**Environment.** As Day 2. `make stress` and no-icount runs use QEMU's multi-threaded mode on the arm64 host.

**Order of work.** `docs/ring.md` was written and committed before any ring code.

### Finding 1 -- the pipeline works end to end

- **Observed.** `make test` under `-icount`: 8000 records went through the rings, encoding, UART1 and the Python decoder.
  0 drops and 0 damaged frames. The digest of decoded records matched the E51's digest of the raw records on all 4 harts.
  The same held in the no-icount run. Wall time for `make test` is 26.7 s against a 120 s timeout.
- **Drops.** `make test-drops` (32-slot rings, 100 ms drain) dropped 1359-1360 of 2000 samples per hart. On every hart,
  `drained + dropped == attempts`, and `seq` gaps equal the drop counter reported in the frames.
- **Tick gaps under `-icount` widened again**, from 873-1098 us to 777-1223 us, now that the E51 runs real work. This fits
  Day 2's scheduling explanation, but this specific change was not tested.

### Finding 2 -- ring tests: logic is well covered, ordering on RISC-V is not

- **Stress results (no icount).** Full-ring regime: 3 runs, 60,000,813 records verified, 0 bad, rings at capacity. Near-empty
  regime: 3 runs, 15,000,102 verified, 0 bad, rings holding 0-1 records at about 99% of polls. In every run, `seq` gaps
  equalled the drop counter on every hart. Hart 2 passed 2^32 attempts in all three full-ring runs, so `seq` really wrapped.
- **Planted bugs.** ThreadSanitizer caught all four removed barriers. The RISC-V stress test caught the logic bug
  (commit before fill) and **none** of the four removed barriers, in either regime.
- **Interpretation.** QEMU did not produce the reordering a RISC-V core may. A stress pass is evidence about logic only. Full
  table in `docs/ring.md`.
- **Decision.** The ordering claim rests on the written argument and the disassembly review, and the docs say so.
- **Not done.** Real hardware, or a tool that models the RISC-V memory model.
- **Observed but not explained.** Hart 2 made about 6 times as many attempts as hart 1 with identical code and pacing.
  Near-empty runs still reached 64 records occasionally. **Hypothesis (not tested):** host threads being paused.

### Finding 3 -- measured wire size

- **Observed.** 15.84 bytes/sample at 1 kHz under `-icount`, against 64 raw. 63,384 bytes per virtual second for 4 harts.
  Full table in `docs/DESIGN.md`.
- **Surprise.** Bytes/sample falls as the sample rate rises under `-icount` (18.87, 15.84, 13.88). It is also lower
  without `-icount` at 1 kHz (12.78). **Hypothesis (not tested):** smaller deltas between samples, and at 100 Hz, the fixed
  cost of the last partial frames and END frames spread over only 800 samples.
- **Decision.** Self-contained frames at 16 samples, based on the measured 4/16/64 comparison.
- **Limit.** A real 115200 baud UART carries 11.52 KB/s (calculated). The 100 Hz rate fits. The 1 kHz rate does not.

### Finding 4 -- two runs did not give results

- **20 kHz without `-icount` failed** the firmware's timer check (mean rate off by more than 5%). Wire rate there is not
  measured. Tick delivery on the host clock could not hold 20 kHz here.
- **Stress under `-icount` is very slow.** A 2,000,000-record target hit the 600 s timeout. A 20,000-record target passed in
  101 s, about 200 records/s. It is slow, not hung. It has little value anyway, since harts take turns.

### Do the tests catch bugs?

Planted in a throwaway copy, beyond the ring table in `docs/ring.md`:

| Planted bug | Caught by |
| --- | --- |
| END frame reports wrong attempts | Decoder |
| Drain skips consuming a record | Firmware self-test |
| Encoder forgets to update `prev` | Decoder |
| `mcycle` and `minstret` swapped on the wire | Decoder digest only |
| `frame_seq` not incremented | Decoder |
| Frame deleted from a capture | Decoder |
| One bit flipped in a capture | Decoder |

### Defects

- First stress test verified only 406,779 records: producers hit a full ring so cheaply that they finished quickly. Fixed by
  running until a target number of records is verified.
- Stress failed 3 times at 20M: hart 2 passed 2^32 attempts and the 32-bit `seq` wrap looked like going backwards. Every
  record was correct. The bug was in the test, and `host/check_stream.py` had the same one. Both now use wrap-safe differences.
- The stress regime first kept every ring full, so the near-empty case was hardly tested. `max_occ` exposed it. Added
  `STRESS_PACE`.
- `python3 -I` broke the decoder's import in `make test`. Removed.
- A TSan mutation run took over 4 minutes reporting races. Added `halt_on_error` and a time limit.
- A `-Wmaybe-uninitialized` warning in `check_samples` appeared with the ring. Fixed by initialising the variables.

---

## Day 4

**Environment.** As Day 3, plus `timescale/timescaledb:2.17.2-pg16` and pyelftools 0.30. Work done unattended on branch
`days-4-7`. Decisions are in `docs/DECISIONS.md`.

### Finding 1 -- hot functions from a real run

- **Observed.** A `make test` capture (1 kHz, `-icount`) was loaded as run 2: 8000 samples, 0 damaged frames. The top function
  on each hart is its workload: `wl_alu` 99.95%, `wl_branch` 99.90%, `wl_stride` 99.85%, `wl_store` 99.70%. Raw output:
  `docs/data/day4_hot_functions.txt`.
- **Limit.** Each hart runs one tight loop, so this confirms the path works. It is not a demanding profiling test. Sampling
  estimates time per function, it does not measure it.

### Finding 2 -- symbolization

- **Test.** Function and line lookups compared with binutils `addr2line` over the real ELF: all agreed (test requires more
  than 500 compared addresses).
- **Defect.** Assembly functions had no `.type`/`.size`, so they were missing from the symbol lookup. Added them to
  `workloads.S` and `trap_entry`.
- **Test defect.** My test expected a function's first address to be on its declaration line. It is on the `{`.

### Problems

- `git push` hung in the macOS keychain helper, probably on a dialog nobody could answer. See D1.
- The image took free disk from 15 GiB to 8.9 GiB, close to the 8 GiB floor. See D2.
- The DB test file was first collected by `make test`, where no capture exists yet. It is now excluded there.

---

## Day 6

Done before Day 5, as the owner ordered. Full tables are in `docs/benchmarks.md`. All measured under `-icount` unless stated.

### Finding 1 -- the gate works

- **Result.** Clean code passes. One `nop` in the arithmetic loop fails at +12.500%. A stride change with the same instruction
  count fails on the DTLB metrics at about +100%. Clean code passes again. Repeat runs show +0.000%.

### Finding 2 -- ISR cost and overhead

- **Measured.** 112 instructions per tick for trap and timer, 194 with sampling, 250 with the checksum. Overhead is the same on all six
  workloads. It crosses 1% between 50,000 Hz (0.9795%) and 52,631.6 Hz (1.0315%).
- **Limit.** That is a share of retired instructions on QEMU. It is not a time overhead on silicon.

### Finding 3 -- the E51 is the first limit

- **Measured.** Drain costs 861.7-969.1 instructions per sample with the checksum off. CRC 20.01 and COBS 13.04 instructions per byte.
  With four harts ticking, no drops up to 20 kHz, then 16.99-19.20% at 50 kHz and 53.59-58.41% at 100 kHz.
- **Consequence.** The system loses samples well below the rate at which the workload hart reaches 1% overhead.

### Finding 4 -- the wall-clock job misses small changes

- **Observed.** Adding a `nop` to the loop (+12.5% instructions) changed host runtime by +0.56%.
- **Hypothesis.** QEMU emits no host code for a `nop`.
- **Test.** Replaced it with a real `add`: +0.30%.
- **Result.** Hypothesis rejected. Cause not explained. 25% more iterations did register: +23.04%, p below 0.000001.

### Finding 5 -- extra misses scale with ticks

- **Observed.** On `load4k` and `store4k`, extra DTLB read misses were 3.0 per tick at each rate. This supports the inference that
  the ISR's own accesses miss, but which access is still not isolated.
- **Not explained.** Extra ITLB misses do not scale the same way, and on `load64` at 100 kHz they were negative (-105).

### Defects

- A requested 52,632 Hz became an 18 us period (55,556 Hz) through integer division. Noticed because the achieved rate was
  reported. Rerun by exact period. This is why tables show achieved rates.
- Two of my own unit-test expectations in `test_wallclock.py` were wrong: one case was just over 3%, and one "not significant"
  case was significant. The code was right.
- `actionlint` was unavailable. See D3.

