# HartWatch design

Everything measured here is from QEMU 8.2.2. Module notes in `docs/` explain each part in plain language.

## Architecture

Performance counters are read on the core being measured, so sampling cost lands on that core. HartWatch keeps that cost to a
counter snapshot and a ring write, and moves everything else to the monitor core.

```
U54 harts 1-4                    L2-LIM              E51 hart 0                 UART1
timer ISR -> snapshot + mepc --> SPSC ring per hart --> drain, delta, varint, --> capture file
             (194 instructions                          CRC-32, COBS
              per tick on QEMU)

Host:  decoder -> symbolizer (ELF/DWARF) -> TimescaleDB -> REST API -> dashboard

Separate path, with sampling off:  benchmark image -> counters on the console -> CI gate
```

| Part | Runs on | Code | Note |
| --- | --- | --- | --- |
| Counter access | U54 | `fw/src/hpm/` | `docs/hpm_backend.md` |
| Sampling ISR | U54 | `fw/src/sampler.c`, `sample.c`, `timer.c` | `docs/sampler.md` |
| Ring | U54 and E51 | `fw/src/ring.h` | `docs/ring.md` |
| Drain | E51 | `fw/src/drain.c` | `docs/drain.md` |
| Encoding and framing | E51 | `fw/src/wire.c` | `docs/wire.md` |
| Decoder | Host | `host/decoder.py`, `check_stream.py` | `docs/decoder.md` |
| Symbolization | Host | `host/symbols.py` | `docs/symbols.md` |
| Collector and schema | Host | `host/collector.py`, `host/sql/` | `docs/collector.md` |
| API and dashboard | Host | `host/api.py`, `host/static/` | `docs/api.md` |
| Benchmarks and gate | QEMU and host | `fw/bench/`, `host/gate.py`, `host/wallclock.py` | `docs/benchmarks.md` |

**The CI gate does not read the telemetry stream.** It needs sampling disabled, so it uses a separate benchmark image and reads
counters from the console.

## Sample record

One record per tick per U54: 64 bytes, aligned to 64, little-endian.

| Offset | Size | Field | Meaning |
| --- | --- | --- | --- |
| 0 | 4 | `seq` | Counts every sample taken, including dropped ones |
| 4 | 1 | `hart` | Hart that took the sample |
| 5 | 1 | `flags` | Reserved, 0 |
| 6 | 2 | `check` | Test instrumentation: `sample_checksum()` if built with `SAMPLE_CHECKSUM=1`, otherwise 0 |
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
Measured wire sizes are in the next section.

## Wire format

UART1 carries a sequence of frames. Each frame is `COBS(payload + CRC) 0x00`.

- **Varint:** unsigned LEB128. 7 bits per byte, least significant group first. The top bit means more bytes follow. Up to 10
  bytes. Example: 300 is `ac 02`.
- **Zigzag:** `(v << 1) ^ (v >> 63)`, on a 64-bit signed value. 0, -1, 1, -2, 2 become 0, 1, 2, 3, 4.
- **CRC:** CRC-32/ISO-HDLC, the same as zlib's `crc32` (polynomial `0xEDB88320` reflected, initial value and final xor
  `0xFFFFFFFF`). It covers the payload, and is appended as 4 bytes, little-endian. Check value: `"123456789"` gives `0xCBF43926`.
- **COBS:** applied to payload plus CRC. Each block starts with a code byte `n`: `n - 1` data bytes follow, then an implied zero,
  except that code `0xFF` means 254 data bytes with no implied zero. The result contains no `0x00`, so `0x00` ends the frame.
  Overhead is at most one byte per 254, plus one.

**Header, all frames:**

| Field | Encoding |
| --- | --- |
| `type` | 1 byte: 1 = samples, 2 = END |
| `hart` | 1 byte |
| `frame_seq` | Varint. Per hart, starts at 0, +1 per frame. A gap means a lost frame |
| `dropped` | Varint. The hart's cumulative drop counter when the frame was built |

**Samples frame:** the header, then records until the end of the payload (up to `FRAME_SAMPLES` = 16). Each record is 8
zigzag varints in this order: `seq`, `mtime`, `mcycle`, `minstret`, `hpm[0]`, `hpm[1]`, `hpm[2]`, `mepc`. Each value is the
difference from the same field of the previous record in this frame, modulo 2^64. For the first record, the previous
values are 0, so frames stand alone. `flags` and `check` are not sent.

**END frame:** the header, then varint `attempts`, varint `drained`, varint `bad`, 1 byte `has_digest`, varint `digest`.
`digest` starts at 0 and folds in each record as it leaves the ring: `digest = rotl(digest, 7) ^ sample_digest(record)`.
It is 0 with `has_digest` = 0 when built with `SAMPLE_CHECKSUM=0`.

### Worked example

A real END frame from a 1 kHz capture, 25 bytes on the wire:

```
wire:     04 02 01 7d 05 d0 0f d0 0f 0f 01 95 ee 9c c2 c8 96 96 cd 05 31 45 26 41 00
payload:  02 01 7d 00 d0 0f d0 0f 00 01 95 ee 9c c2 c8 96 96 cd 05 31 45 26 41
```

COBS: `04` means 3 data bytes (`02 01 7d`), then a zero. `05` means 4 data bytes (`d0 0f d0 0f`), then a zero. `0f` means 14 data
bytes to the end. The last `00` is the delimiter.

| Payload bytes | Field | Value |
| --- | --- | --- |
| `02` | `type` | END |
| `01` | `hart` | 1 |
| `7d` | `frame_seq` | 125 |
| `00` | `dropped` | 0 |
| `d0 0f` | `attempts` | 2000 |
| `d0 0f` | `drained` | 2000 |
| `00` | `bad` | 0 |
| `01` | `has_digest` | yes |
| `95 ee 9c c2 c8 96 96 cd 05` | `digest` | 403732649006806805 |
| `31 45 26 41` | CRC-32 | little-endian |

### Measured wire size

All 4 harts, each on a different workload, 16 samples per frame unless noted. Bytes are counted from the capture, and
include framing, CRC, the final partial frames and END frames. Raw: `docs/data/wire_rates.txt`.

| Sample rate | Mode | Bytes/sample | Bytes/s, all harts |
| --- | --- | --- | --- |
| 100 Hz | `-icount` | 18.87 | 7,573 per virtual second |
| 1 kHz | `-icount` | 15.84 | 63,384 per virtual second |
| 20 kHz | `-icount` | 13.88 | 1,110,264 per virtual second |
| 100 Hz | no icount | 16.37 | 6,588 per host second |
| 1 kHz | no icount | 12.78 | 50,967 per host second |
| 20 kHz | no icount | not measured | Run failed: QEMU's host-timed ticks missed the 5% rate check |

A raw record is 64 bytes. These rates are what the firmware wrote to QEMU's UART, which has no baud limit, and they depend
on the workload. At 1 kHz under `-icount`, per-hart size ranged from 14.61 (alu) to 17.60 (store) bytes/sample.

Samples per frame, 1 kHz, `-icount`: 4 gives 20.23, 16 gives 15.84, 64 gives 14.81 bytes/sample.

Where the bytes go, 1 kHz, `-icount`, excluding the first sample of each frame: `mcycle` and `minstret` take about 3 bytes
each, `mtime` 2, and `seq`, `hpm[2]` and `mepc` about 1 each. `hpm[0]` and `hpm[1]` take 1 to 3 depending on the workload.
On QEMU `mcycle` and `minstret` are one clock, so about 3 bytes per sample are redundant there. They are kept because
`hpm_sifive` needs both.

## CI gate

IPC is flat on QEMU, so there is no IPC gate (`docs/ENGINEERING_LOG.md`, Day 2).

- **Gate, under `-icount`:** `minstret` for a fixed amount of work on one U54 with the E51 asleep and **sampling
  disabled**, plus DTLB read and write misses per thousand instructions. Plain threshold against a stored baseline,
  default 3%.
- **Why sampling is off:** with it on, the ISR's own accesses add DTLB misses (inferred, not isolated; 94,536 read
  misses on the store hart).
  That extra is overhead, reported in the Day 6 sweep. It must not be mixed into a workload metric.
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
| Cost of an instruction | 1 ns each under `-icount`, by definition | Varies. CSR reads and MMIO can cost far more |
| Tick lateness | Set by QEMU's hart rotation under `-icount` | Interrupt latency, not measured |
| ISR cost | 194 instructions per tick, checksum off | Instruction count should be similar. Time not measured |
| Counters per hart | Not per-hart under `-icount`: other harts inflate them | Per-hart |
| Counters while in `wfi` | `minstret` keeps rising | Should not rise |
| Event counters | `mhpmcounter3`-`18` | Believed to be 2 |
| Events | DTLB read/write and ITLB fills of QEMU's software TLB | SiFive selectors, unverified |
| `mcountinhibit` | Traps as illegal | Not measured |
| Caches | Not modelled | Real, and affect timing |
| UART baud rate | Divisor is not implemented, per the note in `uart.c`; not measured here | Must be programmed; 115200 baud carries 11.52 KB/s |
| L2-LIM | Holds all 4 rings; used concurrently by 5 harts | Size, caching and coherence not measured |
| Memory ordering | Removing ring barriers changed nothing in stress runs | RISC-V allows reordering QEMU did not show |
| UART1 throughput | 1,110,264 bytes per virtual second reached, no limit seen | 115200 baud carries 11.52 KB/s (calculated) |

## Ring memory ordering: what the tests cannot prove

**Limitation.** Host-side stress tests and ThreadSanitizer cannot prove that the ring's acquire/release is sufficient on
RISC-V. An x86 host has stronger memory ordering than RISC-V: it does not reorder most of what RISC-V may, so a missing
barrier can pass every run. This project's current build host is arm64, which is weaker than x86 but is still not
RISC-V. ThreadSanitizer checks the program against the C11 model on the host it runs on. It says nothing about the
machine code on a RISC-V core. QEMU does not reproduce a RISC-V core's reordering either. Passing tests are evidence of
no bug found, so the design rests on the argument below, which is expanded in `docs/ring.md`.

**The argument in words.**

1. Each index has one writer: only the producer writes `head`, only the consumer writes `tail`.
2. The producer fills a slot first, then publishes it with a store-release of `head`. Release means no earlier write
   may become visible after that store.
3. The consumer load-acquires `head`. Acquire means no later read may happen before that load. So if it sees the new
   `head`, it must also see the finished slot.
4. The same pair runs the other way. The consumer finishes reading a slot, then store-releases `tail`. The producer
   load-acquires `tail` before reusing that slot, so it cannot overwrite a record still being read.
5. Neither side ever waits for the other, and neither writes the other's index, so no atomic read-modify-write is needed.

The compiler turns these into RISC-V `fence` instructions or their equivalents. Checking that output is a disassembly
review, recorded in `docs/ring.md`.

**Measured confirmation of this limitation.** With each barrier removed in turn, ThreadSanitizer reported a race every
time, and the RISC-V stress test under QEMU passed every time. See the table in `docs/ring.md`.

## The real-UART limit

115200 baud with 8N1 framing carries 11.52 KB/s (calculated: 10 bits per byte). The measured stream at 1 kHz with 4 harts is 63,384
bytes per virtual second under `-icount`. That is about 5.5 times more than this link carries, so this design at 1 kHz could not
run over a real 115200 baud UART. The 100 Hz stream (7,573 bytes per virtual second) would fit.

QEMU's UART has no baud limit, so this never appears in any run here. Real hardware would need a lower sample rate, a faster link,
fewer fields, or batching on the host side. `uart_putc` would also spin on a full FIFO, which would make the E51 the bottleneck and
turn UART speed into ring drops.

## Porting notes

**Everything in this section is from memory and untested on hardware.** Check each item against the vendor's manual first.

### FU540 (`hpm_sifive`)

- Build with `make HPM_BACKEND=sifive BUILD=build/sifive`. It compiles and links. It has never run.
- `mhpmeventN` selectors, as remembered: event class in bits 7:0, event mask above. Class 0 is instruction commit, class 1 is
  microarchitectural (branch mispredicts, for example), class 2 is the memory system (I-cache, D-cache, TLB misses).
- The U54 is remembered as having only `mhpmcounter3` and `mhpmcounter4`, so the backend fills two of the three slots, and has no
  separate DTLB write-miss event.
- Real IPC would come from here: on silicon `mcycle` and `minstret` should be separate per-hart counters.
- The UART divisor must be programmed. L2-LIM's size, caching and coherence must be checked, because the ring's argument assumes
  coherent memory.

### Arm PMU mapping

| HartWatch | Arm equivalent, from memory |
| --- | --- |
| `mcycle` | `PMCCNTR_EL0` |
| `minstret` | Event counter with event `0x08` (`INST_RETIRED`) |
| `mhpmcounterN` and `mhpmeventN` | `PMEVCNTR<n>_EL0` and `PMEVTYPER<n>_EL0` |
| DTLB miss | Event `0x05` (`L1D_TLB_REFILL`) |
| ITLB miss | Event `0x02` (`L1I_TLB_REFILL`) |
| Enable counters | `PMCR_EL0`, `PMCNTENSET_EL0` |
| `mtime` | `CNTVCT_EL0` or `CNTPCT_EL0` |
| `mtimecmp` | `CNTP_CVAL_EL0` |
| `mepc` | `ELR_EL1` (or the level the handler runs at) |
| Store-release and load-acquire | `STLR` and `LDAR` |

Only the `hpm_backend` files and the timer and trap code would change. The ring is written in C atomics. Its ordering argument
needs rechecking for Arm's memory model, though the same release/acquire pairing applies.
