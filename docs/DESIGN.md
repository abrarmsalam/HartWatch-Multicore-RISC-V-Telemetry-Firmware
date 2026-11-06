# HartWatch design

Later days add to this document. Sections on the wire format and Arm PMU mapping are not written yet.

## Sample record

One record per tick per U54: 64 bytes, aligned to 64, little-endian.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `seq` | Counts every sample taken, including dropped ones |
| 4 | 1 | `hart` | Hart that took the sample |
| 5 | 1 | `flags` | Reserved, 0 |
| 6 | 2 | `check` | `sample_checksum()` of everything else |
| 8 | 8 | `mtime` | Sample time on the clock shared by all harts, read in the ISR |
| 16 | 8 | `mcycle` | |
| 24 | 8 | `minstret` | |
| 32 | 8 | `hpm[0]` | DTLB read miss |
| 40 | 8 | `hpm[1]` | DTLB write miss |
| 48 | 8 | `hpm[2]` | ITLB miss |
| 56 | 8 | `mepc` | Interrupted instruction |

**Why not 32 bytes:** six 64-bit values are already 48 bytes, and a sequence number and timestamp are also needed. The
32-byte option truncated every counter to 32 bits. The 48-byte option had no room for a sequence number or timestamp,
and half of its records would straddle two cache lines.

**Checksum:** `acc` starts as `seq | hart << 32 | flags << 40`. For `mtime`, `mcycle`, `minstret`, `hpm[0..2]`, `mepc` in
that order, `acc = rotl(acc, 7) ^ field`. The result is `acc ^ acc >> 16 ^ acc >> 32 ^ acc >> 48`, truncated to 16 bits.

**Raw ring rate, 4 harts (calculated, not measured):** 25.6 KB/s at 100 Hz, 256 KB/s at 1 kHz, 5.12 MB/s at 20 kHz.
Encoded size on the wire: not measured.

## CI gate

IPC is flat on QEMU, so there is no IPC gate (`docs/ENGINEERING_LOG.md`, Day 2).

- **Gate, under `-icount`:** `minstret` for a fixed amount of work on one U54 with the E51 asleep, plus DTLB read and
  write misses per thousand instructions. Plain threshold against a stored baseline, default 3%.
- **Why no statistical test there:** repeat `-icount` runs were identical, so variance is zero and a rank test has
  nothing to work with.
- **What it cannot see:** stalls. It catches added instructions and added page-level memory traffic.
- **Mann-Whitney U:** only in a separate no-icount wall-clock job, which measures QEMU host runtime, not target
  performance.

## QEMU versus silicon

The QEMU column is measured on QEMU 8.2.2. Nothing has been run on silicon. That column is what the RISC-V spec or
memory of the FU540 manual suggests, to be verified.

| Topic | QEMU `sifive_u` (measured) | FU540 silicon (not measured) |
| --- | --- | --- |
| `mcycle` | Virtual time under `-icount`, host time without | Core clock cycles |
| `minstret` | Same clock as `mcycle` | Retired instructions |
| IPC | 1.000 under `-icount`, 0.994-1.000 without | Should vary with workload |
| Counters per hart | Not per-hart under `-icount`: other harts inflate them | Per-hart |
| Counters while in `wfi` | `minstret` keeps rising | Should not rise |
| Event counters | `mhpmcounter3`-`18` | Believed to be 2 |
| Events | DTLB read/write and ITLB fills of QEMU's software TLB | SiFive selectors, unverified |
| `mcountinhibit` | Traps as illegal | Not measured |
| Caches | Not modelled | Real, and affect timing |
| UART baud rate | Divisor is not implemented, per the note in `uart.c`; not measured here | Must be programmed; 115200 baud carries 11.52 KB/s |
| L2-LIM | 512,000 bytes written and read back correctly (1 MiB reserved) | Size and behaviour not measured |

## Porting notes

- Selecting a backend: `make HPM_BACKEND=sifive BUILD=build/sifive`. It has never run on hardware.
- FU540: check `SIFIVE_EVENT_*` and the number of event counters against the manual before trusting `hpm_sifive.c`.
- Arm PMU mapping: not written yet.
