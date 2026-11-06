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
  today. With busy U54s they are 873-1098 us. Mean is still 999-1000 us and 0 ticks are late.
  **Hypothesis (not tested):** QEMU's round-robin scheduling of busy harts delays when a tick is noticed.
- **Surprise 2: read and write misses now cross over slightly.** The probe, with no ISR running, showed exactly 0 on
  the other event. With sampling on, the store hart shows 94,536 read misses, about 0.1% of its write misses.
  **Hypothesis (not tested):** the ISR's own memory accesses miss because the workload evicted their TLB entries.
- **Surprise 3: ITLB miss moved for the first time.** About 26,000 on the load and store harts, 0 on alu and branch.
  The probe's reading was 0-23. **Hypothesis (not tested):** the same eviction, hitting the code's TLB entries. So the
  ITLB slot responds to something, but not yet to code footprint, which is what it would normally be for.
- **No-icount reminder.** Gaps ranged from 1 us to 126,741 us and 18-113 ticks per hart were late. Miss counts were lower
  than under `-icount` and varied between runs. The time-based test limits passed, but these runs say nothing about
  timing quality.

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
