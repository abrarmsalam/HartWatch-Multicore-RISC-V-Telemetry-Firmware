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
