# Morning report: Days 4-7

Unattended run. All numbers come from runs recorded in `docs/data/`. All describe QEMU 8.2.2, not hardware. Build host: arm64 Mac, in the
`hartwatch-dev` Ubuntu 24.04 container, unless stated. Decisions taken while you slept are in `docs/DECISIONS.md` (D1-D8).

## 1. Status

| Day | State | Reason for anything incomplete |
| --- | --- | --- |
| 4: collector, schema, symbolization | **Done** | The collector reads finished captures only, with no live follow mode |
| 6: benchmarks, sweep, CI gate | **Done** | `actionlint` not used: not installed, and downloading it was blocked (D3). GitHub ran the workflow instead |
| 5: API and dashboard (minimal) | **Partly done** | No WebSocket: it would only replay stored rows (D8). Dashboard was served, but never viewed in a browser |
| 7: documentation | **Done** | No v1.0 tag, as you ordered |

Other things you should know:

- **Not merged.** All of this is on `days-4-7`. `main` has the Day 3 commits.
- **Pushing.** `git push` hung in the macOS keychain helper, so pushes used your `gh` login for each command (D1). The next normal
  `git push` from this machine may show a keychain dialog.
- **Disk.** Free space went from 15 GiB to 8.9 GiB, mostly the TimescaleDB image (2.74 GB unpacked) and Docker's disk file growing. It
  stayed above the 8 GiB floor. Nothing was pruned or deleted.
- **Left running.** Docker Desktop. The `db` and `api` containers are stopped, not removed. The database volume holds runs 2, 4 and 5.

## 2. Test output

Final `make test`, under `-icount`, on commit `b4b4166`:

```
host unit tests (backend: fake, SAMPLE_CHECKSUM=1)
  test_layout
  test_fill_copies_every_field
  test_checksum_golden
  test_checksum_catches_every_single_bit_flip
  test_checksum_catches_swapped_fields
  test_check_next_accepts_good_stream
  test_check_next_rejects_bad_stream
  test_tick_uses_backend_and_the_right_hart
  test_tick_when_full_counts_a_drop_and_skips_the_snapshot
557 checks, 0 failed
host unit tests (backend: fake, SAMPLE_CHECKSUM=0)
  test_layout
  test_fill_copies_every_field
  test_corruption_is_not_detected_without_checksum
  test_check_next_accepts_good_stream
  test_check_next_rejects_bad_stream
  test_tick_uses_backend_and_the_right_hart
  test_tick_when_full_counts_a_drop_and_skips_the_snapshot
44 checks, 0 failed
ring unit tests
  test_layout
  test_init_rejects_bad_capacity
  test_empty
  test_fifo_order
  test_peek_does_not_consume
  test_reserve_without_commit_publishes_nothing
  test_every_slot_is_usable
  test_full_drops_and_keeps_old_records
  test_space_returns_after_consume
  test_wraps_many_times
  test_indices_near_the_top_of_their_range
60 checks, 0 failed
wire unit tests
  test_zigzag
  test_varint
  test_crc
  test_cobs_published_examples
  test_cobs_long_runs
  test_frame_has_no_zero_until_the_end
  test_worst_case_frame_fits
588 checks, 0 failed
........................................................................ [ 81%]
................                                                         [100%]
88 passed in 2.79s
attempts 2000000 received 886386 dropped 1113614 seq_gaps 1113614 bad 0

=== HartWatch firmware ===
hart 0: monitor core up, isa=rv64acimu
hart 0: all 5 harts are online
  hart 0  isa=rv64acimu
  hart 1  isa=rv64acdfimsu
  hart 2  isa=rv64acdfimsu
  hart 3  isa=rv64acdfimsu
  hart 4  isa=rv64acdfimsu
hart 0: waiting for each U54 to take 2000 timer interrupts at 1000 Hz...

 hart  ticks  mean_gap_us  min_gap_us  max_gap_us  late(>2x)
    1   2000          999         882        1118          0
    2   2000          999         902        1098          0
    3   2000         1000         921        1079          0
    4   2000          999         777        1223          0

 counter backend: qemu
 hart workload samples drops  bad mepc_in_wl dtlb_read dtlb_write  itlb
    1      alu    2000     0    0       1999        72         95     8
    2   branch    2000     0    0       1998         0         31     0
    3     load    2000     0    0       1997 101185735       7941 24265
    4    store    2000     0    0       1994    186983   89750115 24190
 wire: 504 frames, 126723 bytes on UART1

ALL TESTS PASSED
-----------------------------------------------------------
PASS
 hart  samples  dropped  frames  wire_bytes  bytes/sample  span_us  bytes/s
    1     2000        0     126       29215         14.61  1998928    14615
    2     2000        0     126       29238         14.62  1998946    14627
    3     2000        0     126       33075         16.54  1999001    16546
    4     2000        0     126       35195         17.60  1998872    17607
 all      8000  bytes 126723  bytes/sample 15.84  span_us 1999288  bytes/s 63384
DECODER PASS
make test rc=0
```

`make test-drops` and `make gate` also passed on that commit, with 0 compiler warnings.

**GitHub Actions** ran on `ubuntu-24.04`, `x86_64`, QEMU 8.2.2, GCC 13.2.0:

| Run | Commit | Result |
| --- | --- | --- |
| 42172406712 | `4a367c9` | success, both jobs |
| 42172788467 | `fc19fa0` | `test-and-gate` success. `qemu-host-runtime` cancelled after about 15 minutes in its `apt` install step, which matches its `timeout-minutes` (inferred: an install stall, not a code failure) |
| 42173085324 | `b4b4166` | success, both jobs: 88 pytest passed, gate passed, `make test` 504 frames and 126,723 bytes |

### Gate demonstration

Measured under `-icount`, exact output from `docs/data/gate_demo.txt`:

```
### 1. Clean code
workload  metric           baseline         current          change   (threshold +3.0%)
arith     minstret          40000015.0000    40000015.0000    +0.000%  ok
arith     dtlb_read_pki            0.0000           0.0000        n/a  ok
arith     dtlb_write_pki           0.0000           0.0000        n/a  ok
branch    minstret          48000016.0000    48000016.0000    +0.000%  ok
branch    dtlb_read_pki            0.0000           0.0000        n/a  ok
branch    dtlb_write_pki           0.0000           0.0000        n/a  ok
load4k    minstret          35000021.0000    35000021.0000    +0.000%  ok
load4k    dtlb_read_pki          142.8571         142.8571    +0.000%  ok
load4k    dtlb_write_pki           0.0000           0.0000        n/a  ok
load64    minstret          35000020.0000    35000020.0000    +0.000%  ok
load64    dtlb_read_pki            2.2322           2.2322    +0.000%  ok
load64    dtlb_write_pki           0.0000           0.0000        n/a  ok
store4k   minstret          35000021.0000    35000021.0000    +0.000%  ok
store4k   dtlb_read_pki            0.0000           0.0000    +0.000%  ok
store4k   dtlb_write_pki         142.8571         142.8571    +0.000%  ok
store64   minstret          35000020.0000    35000020.0000    +0.000%  ok
store64   dtlb_read_pki            0.0000           0.0000    +0.000%  ok
store64   dtlb_write_pki           2.2321           2.2321    +0.000%  ok
GATE PASS: 18 metrics within +3.0%
exit code: 0

### 2. Deliberate slowdown: one nop in the arithmetic loop (-DWL_SLOWDOWN_ALU)
workload  metric           baseline         current          change   (threshold +3.0%)
arith     minstret          40000015.0000    45000015.0000   +12.500%  FAIL
arith     dtlb_read_pki            0.0000           0.0000        n/a  ok
arith     dtlb_write_pki           0.0000           0.0000        n/a  ok
branch    minstret          48000016.0000    48000016.0000    +0.000%  ok
branch    dtlb_read_pki            0.0000           0.0000        n/a  ok
branch    dtlb_write_pki           0.0000           0.0000        n/a  ok
load4k    minstret          35000021.0000    35000021.0000    +0.000%  ok
load4k    dtlb_read_pki          142.8571         142.8571    +0.000%  ok
load4k    dtlb_write_pki           0.0000           0.0000        n/a  ok
load64    minstret          35000020.0000    35000020.0000    +0.000%  ok
load64    dtlb_read_pki            2.2322           2.2322    +0.000%  ok
load64    dtlb_write_pki           0.0000           0.0000        n/a  ok
store4k   minstret          35000021.0000    35000021.0000    +0.000%  ok
store4k   dtlb_read_pki            0.0000           0.0000    +0.000%  ok
store4k   dtlb_write_pki         142.8571         142.8571    +0.000%  ok
store64   minstret          35000020.0000    35000020.0000    +0.000%  ok
store64   dtlb_read_pki            0.0000           0.0000    +0.000%  ok
store64   dtlb_write_pki           2.2321           2.2321    +0.000%  ok
GATE FAIL: 1 metric(s) over threshold, 0 problem(s)
make: *** [Makefile:150: gate] Error 1
exit code: 2

### 3. Regression with the same instruction count: stride 64 -> 128 bytes (-DBENCH_SMALL_STRIDE=128)
workload  metric           baseline         current          change   (threshold +3.0%)
arith     minstret          40000015.0000    40000015.0000    +0.000%  ok
arith     dtlb_read_pki            0.0000           0.0000        n/a  ok
arith     dtlb_write_pki           0.0000           0.0000        n/a  ok
branch    minstret          48000016.0000    48000016.0000    +0.000%  ok
branch    dtlb_read_pki            0.0000           0.0000        n/a  ok
branch    dtlb_write_pki           0.0000           0.0000        n/a  ok
load4k    minstret          35000021.0000    35000021.0000    +0.000%  ok
load4k    dtlb_read_pki          142.8571         142.8570    -0.000%  ok
load4k    dtlb_write_pki           0.0000           0.0000        n/a  ok
load64    minstret          35000020.0000    35000020.0000    +0.000%  ok
load64    dtlb_read_pki            2.2322           4.4643   +99.999%  FAIL
load64    dtlb_write_pki           0.0000           0.0000        n/a  ok
store4k   minstret          35000021.0000    35000021.0000    +0.000%  ok
store4k   dtlb_read_pki            0.0000           0.0000    +0.000%  ok
store4k   dtlb_write_pki         142.8571         142.8570    -0.000%  ok
store64   minstret          35000020.0000    35000020.0000    +0.000%  ok
store64   dtlb_read_pki            0.0000           0.0000    +0.000%  ok
store64   dtlb_write_pki           2.2321           4.4643  +100.000%  FAIL
GATE FAIL: 2 metric(s) over threshold, 0 problem(s)
make: *** [Makefile:150: gate] Error 1
exit code: 2

### 4. Clean code again
GATE PASS: 18 metrics within +3.0%
exit code: 0
```

## 3. Every measured number

### Under `-icount`

**Counters (one hart, others asleep).** IPC 1.000 on every workload. `minstret` within 16 instructions of the true count on 35-48 million.
With other harts busy, hart 1's alu run read 208,000,004-292,000,001 against a true 40,000,000: not per-hart.

**DTLB, sampling off, per 5,000,000 accesses.** Stride 64 B: 78,125 misses. Stride 4 KiB + 64 B: 5,000,000. Reads on loads, writes on stores,
0 on the other event. Arithmetic and branch: 0.

**`make test`, 1 kHz, 2000 ticks per hart.** 8000 samples, 0 drops, digest matches. Tick gaps 777-1223 us. Read misses on the store hart:
186,983.

**Tick lateness (Day 2 experiment), worst on h1/h2.** Busy harts 0, 1, 2, 3, 4: 1, 1, 41, 73, 98 us. Periods 500, 1000, 2000, 4000 us: 49, 98,
196, 391 us. With `-smp` 3, 4, 5 and 2 busy, ticking harts: 334, 250, 201 us. Busy harts with no timer: up to 48,800 us.

**Wire.**

| Rate | Bytes/sample | Bytes per virtual second, 4 harts |
| --- | --- | --- |
| 100 Hz | 18.87 | 7,573 |
| 1 kHz | 15.84 | 63,384 |
| 20 kHz | 13.88 | 1,110,264 |

Samples per frame at 1 kHz: 4, 16, 64 gave 20.23, 15.84, 14.81 bytes/sample. Raw record: 64 bytes.

**ISR cost, instructions per tick.** Trap and timer only: 112. With sampling, checksum off: 194. With checksum: 250.

**Overhead on the workload hart, checksum off, E51 asleep, fixed work.**

| Achieved rate | Overhead | Ticks |
| --- | --- | --- |
| 100.0 Hz | 0.0019% | 40 |
| 1,000.0 Hz | 0.0194% | 400 |
| 5,000.0 Hz | 0.0970% | 2,001 |
| 10,000.0 Hz | 0.1943% | 4,007 |
| 20,000.0 Hz | 0.3895% | 8,031 |
| 50,000.0 Hz | 0.9795% | 20,195 |
| 52,631.6 Hz | 1.0315% | 21,269 |
| 55,555.6 Hz | 1.0895% | 22,464 |
| 58,823.5 Hz | 1.1543% | 23,801 |
| 100,000.0 Hz | 1.9784% | 40,791 |

These are from the arithmetic workload. All six workloads matched to within 0.0001 percentage points at the swept rates. The virtual runtime
of the workload hart is the same figure: 1 instruction = 1 ns here. Two repeated cells were identical.

**Extra misses from sampling.** `load4k`: 3.0 extra DTLB read misses per tick at every rate. Extra at 20 kHz, read / write / ITLB: arith 81 / 189 / 0,
`load4k` 21,081 / 21,020 / 74,690. Full table: `docs/data/overhead_summary.txt`.

**E51 drain.** 861.7-969.1 instructions per sample with the checksum off, 912.4-1,019.8 on. CRC-32: 20.01 instructions per byte. COBS: 13.04.

**Live drain, 4 harts, 4096-slot rings.**

| Requested | Achieved mean | Max occupancy | Drops per hart |
| --- | --- | --- | --- |
| 100 Hz | 100.0-100.1 Hz | 17 | 0 |
| 1 kHz | 1,000.0-1,000.2 Hz | 32 | 0 |
| 5 kHz | 5,000.0-5,000.2 Hz | 96 | 0 |
| 10 kHz | 10,000.0-10,000.2 Hz | 160 | 0 |
| 20 kHz | 19,999.9-20,000.2 Hz | 320 | 0 |
| 50 kHz | 49,999.9-50,000.1 Hz | 4,096 | 16.99-19.20% |
| 100 kHz | 100,000.0-100,000.2 Hz | 4,096 | 53.59-58.41% |

**Hot functions, real run.** `wl_alu` 99.95%, `wl_branch` 99.90%, `wl_stride` 99.85%, `wl_store` 99.70%. Sampling estimates.

**`make test-drops`.** 1,359-1,360 of 2,000 samples dropped per hart, all accounted for.

**Gate.** Clean: +0.000% on every metric with a non-zero baseline. Slowdown: +12.500%. Stride change: +99.999% and +100.000%.

### Without `-icount`

**Counters.** IPC 0.994-1.000. `mcycle` tracks host time. `minstret` is wrong by up to about 1.7 billion.

**Wire.** 100 Hz: 16.37 bytes/sample, 6,588 bytes per host second. 1 kHz: 12.78, 50,967. 20 kHz: **not measured**, the run failed the 5% tick-rate check.

**Ring stress, on RISC-V firmware.** Full-ring regime: 60,000,813 records verified. Near-empty regime: 15,000,102. Total 75,000,915, 0 bad.

**Stress with planted bugs.** Logic bug caught. 0 of 4 removed barriers caught. ThreadSanitizer (host, arm64): 4 of 4 caught.

**Wall-clock job (QEMU host runtime, not target performance).** Identical code: -0.38%, p 0.297677, PASS. Extra `nop`: +0.56%, p 0.440750, PASS.
Extra `add`: +0.30%, p 0.903116, PASS. 25% more iterations: +23.04%, p 0.000000, FAIL.

### Calculated, not measured

115200 baud carries 11.52 KB/s. Raw ring rate for 4 harts: 25.6 KB/s at 100 Hz, 256 KB/s at 1 kHz, 5.12 MB/s at 20 kHz. CRC plus COBS at about 470 of 862
drain instructions.

### Not measured

ISR time on anything real. Interrupt latency. Any cache effect. Anything on silicon. Drop threshold between 20 and 50 kHz. API throughput. Decoder and
collector throughput on large captures. Wire rate at 20 kHz without `-icount`.

## 4. Claims the evidence supports

You can say these, with "on QEMU" attached where it applies:

- Built a bare-metal multi-hart RISC-V telemetry pipeline running on QEMU `sifive_u`: timer-ISR sampling, lock-free SPSC rings, a monitor core that encodes
  and frames data, a Python decoder, TimescaleDB storage, ELF/DWARF symbolization, a REST API, and a CI gate.
- Found by experiment that IPC cannot work as a regression metric on QEMU (1.000 on every workload), traced the reason to QEMU's source, and redesigned the
  gate around a metric that moves.
- The CI gate fails on a deliberate +12.5% instruction regression and on a DTLB regression with the same instruction count, and passes on clean code.
- Sampling costs 194 instructions per tick on the sampled hart on QEMU, with the encoding work moved to the monitor core (861.7-969.1 instructions per sample there).
- End-to-end integrity: a digest of raw records matches decoded values, and every dropped sample is accounted for.
- Encoding reduces 64-byte records to 15.84 bytes per sample at 1 kHz (measured on QEMU with these workloads).
- The ring is argued and TSan-mutation-tested, not proven on RISC-V. 75,000,915 records verified under QEMU with 0 bad.
- Used planted bugs to check the tests themselves. This found a real hole in the self-test, and showed the stress test cannot detect missing barriers.
- Identical stream size and gate results on arm64 and x86_64 hosts running the same QEMU and compiler.
- Traced tick lateness under `-icount` to QEMU's round-robin scheduling, with a prediction that matched (334, 250, 201 us against 333, 250, 200 us), for the
  two-hart case.

## 5. Claims the evidence does NOT support

Please don't say these:

- **"Low overhead on real hardware" or "under 1% overhead at 50 kHz".** The figure is a share of retired instructions on QEMU. It is not time, and nothing ran on silicon.
- **"Runs on the FU540" or "hardware-validated".** `hpm_sifive` compiles and has never run. Selectors and counter counts are from memory.
- **"The ring is proven correct" or "verified on RISC-V".** Removing any barrier still passed the RISC-V stress test.
- **Any claim about IPC.** Real IPC was never measured.
- **"Detects performance regressions" in general.** The gate sees added instructions and page-level traffic in six loops. It cannot see stalls or cache behaviour.
- **"Measures cache behaviour" or "cache-thrashing workloads".** QEMU has no cache model. Miss counts are QEMU's software TLB.
- **"Real-time" or "live" telemetry.** The collector reads finished captures. There is no WebSocket.
- **"1 kHz over a real serial link".** The measured 63 KB/s exceeds 115200 baud's 11.52 KB/s by about 5.5 times.
- **"Handles 20 kHz" as a general statement.** That holds for these workloads under `-icount`. Without `-icount`, 20 kHz failed.
- **"The wall-clock job catches regressions".** It missed a 12.5% instruction increase.
- **"The ISR's own accesses cause the extra misses" as fact.** That is an inference. Scaling with ticks supports it, and no experiment isolated it.
- **"Production-ready".** No authentication, by design, and the dashboard was never viewed in a browser.

## 6. Unexplained findings and open problems

**Unexplained. Left as logged, not chased:**

- Hart 2 makes about 6 times the stress attempts of hart 1 with identical code.
- Tick gaps widened to 777-1223 us once the E51 did real work. This fits the scheduling explanation, but that was not tested.
- Bytes per sample fall as the rate rises. The explanation is an untested hypothesis.
- The exact 98 us lateness with all 4 harts ticking. My model gave 596 us and the wrong harts. Hart 4's outlier of 128 us too.
- The 5-7 times inflation of counters with busy harts, not 4 times.
- Why one extra instruction barely changes QEMU host runtime. My `nop` hypothesis was tested and rejected.
- Extra ITLB misses do not scale with ticks, and were negative (-105) on `load64` at 100 kHz.
- Occasional 64-record bursts in near-empty stress runs. Hypothesis: host threads being paused. Not tested.

**Open problems:**

- The ring's ordering on real RISC-V: needs hardware, or a tool that models the RISC-V memory model.
- Whether L2-LIM is coherent and cached as the ring needs on the FU540: not verified.
- The E51 is the first limit, well below the 1% point, and CRC plus COBS look like much of its cost (calculated). The real UART limit is lower still.
- ITLB slot still unproven: no workload has a large code footprint.
- Stress under `-icount` is too slow to use (about 200 records/s).
- Dashboard rendering not checked. `/docs` needs internet access for Swagger UI.
- `make stress` is not part of `make test` or CI.
- `bench/baseline.json` is tied to this QEMU and compiler version.

## 7. Where things are

- **Branch:** `days-4-7`, pushed to https://github.com/abrarmsalam/HartWatch-Multicore-RISC-V-Telemetry-Firmware
- **Last code commit:** `b4b4166`. Only documentation commits follow it, and CI on those had not finished when this was written. `git log -1 days-4-7` gives the tip.
- **No tag was created.**

**Review first, in this order:**

1. `docs/DECISIONS.md`: eight short decisions I made without you.
2. Section 5 of this report, before writing anything on your resume.
3. `docs/benchmarks.md`: the gate, the sweep, and the meaning of the 1% number.
4. `fw/bench/bench.c`: the modes every Day 6 number depends on.
5. The dashboard in a real browser: `docker compose up -d`, then http://127.0.0.1:8000.
6. Then decide whether to merge `days-4-7` into `main`.
