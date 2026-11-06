# Benchmarks, overhead sweep and the CI gate

All numbers are from QEMU 8.2.2 and describe QEMU. None describe real hardware. Raw data is in `docs/data/`:
`overhead_a.csv`, `overhead_b.csv`, `isr_cost.csv`, `overhead_crossing.txt`, `gate_demo.txt`, `wallclock_demo.txt`.
`scripts/overhead_sweep.py` reruns the sweep, and `scripts/sweep_report.py` prints the derived numbers.

## Why the benchmark image has three modes

Under `-icount`, `minstret` is one global clock. It only means "this hart's instructions" when every other hart is asleep.
`fw/bench/bench.c` is built around that:

| Mode | What runs | What it is for |
| --- | --- | --- |
| 0, gate | Hart 1 alone, no timer, no sampling | The CI gate |
| 1, overhead A | Hart 1 with timer and sampling, E51 asleep, ring large enough not to drop | Exact cost on the workload hart, then drain cost |
| 2, overhead B | Four U54s tick and work, E51 drains live | Ring occupancy, drops, achieved tick rate |

No experiment mixes busy harts without timers with ticking harts.

## The CI gate

`make gate` runs mode 0 under `-icount` and compares three metrics per workload with `bench/baseline.json`: `minstret`, and DTLB
read and write misses per thousand instructions. A rise above the threshold fails (default 3%, set with `GATE_THRESHOLD`).

- **There is no IPC metric.** IPC is flat on QEMU.
- **It is a plain threshold.** Repeat runs are identical, so a statistical test would have no variance to work with.
- **A metric with a zero baseline** has no percentage, so it fails if it rises above 0.001.
- **What it catches:** added instructions and added page-level memory traffic in the benchmark workloads.
- **What it cannot catch:** stalls, cache behaviour, anything about timing on silicon, and regressions outside the code these
  workloads run.

### Proof that it can fail

Real output is in `docs/data/gate_demo.txt`. Measured under `-icount`.

| Run | Result |
| --- | --- |
| Clean code | PASS, 18 metrics |
| One `nop` in the arithmetic loop | FAIL: `arith minstret` 40,000,015 to 45,000,015, +12.500% |
| Stride 64 to 128 bytes, same instruction count | FAIL: `load64 dtlb_read_pki` +99.999%, `store64 dtlb_write_pki` +100.000% |
| Clean code again | PASS, 18 metrics |

The third case is why DTLB is gated too: `minstret` did not change, so an instruction-only gate would have passed it.

## The wall-clock job

`host/wallclock.py` compares two images by timing QEMU with no `-icount`. **It measures QEMU host runtime, not target
performance.** It fails only if Mann-Whitney U is significant (p < 0.05) and the median rises by more than 3%. 20 runs per side,
interleaved, hand-written U with tie and continuity corrections, no SciPy.

| Candidate against baseline | Median change | p | Verdict |
| --- | --- | --- | --- |
| Identical code (control) | -0.38% | 0.297677 | PASS |
| One extra `nop` in the loop (+12.5% instructions) | +0.56% | 0.440750 | PASS |
| One extra `add` in the loop (+12.5% instructions) | +0.30% | 0.903116 | PASS |
| 25% more iterations | +23.04% | 0.000000 | FAIL |

**This job does not notice a 12.5% instruction increase.** My hypothesis was that QEMU emits no host code for a `nop`. The `add`
run rejected that. Why an extra instruction barely changes host time is not explained. In CI this job is informational and
cannot fail the workflow.

## Overhead sweep

Under `-icount`, `SAMPLE_CHECKSUM=0`. Wall-clock overhead can't be measured meaningfully on QEMU, so overhead is reported as
extra retired instructions on the workload hart. Under `-icount`, with the other harts asleep, virtual runtime is the same
number: 1 instruction = 1 ns, so `mcycle` equalled `minstret` in these runs.

### Cost on the workload hart (mode A)

Fixed work: 400,000,013 instructions (arithmetic). Repeat runs of two cells were identical.

| Requested | Achieved rate | Ticks | Extra instructions | Overhead | Drops |
| --- | --- | --- | --- | --- | --- |
| Off | - | 0 | 0 | 0 | - |
| 100 Hz | 100.0 Hz | 40 | 7,756 | 0.0019% | 0 |
| 1 kHz | 1,000.0 Hz | 400 | 77,596 | 0.0194% | 0 |
| 5 kHz | 5,000.0 Hz | 2,001 | 388,190 | 0.0970% | 0 |
| 10 kHz | 10,000.0 Hz | 4,007 | 777,354 | 0.1943% | 0 |
| 20 kHz | 20,000.0 Hz | 8,031 | 1,558,010 | 0.3895% | 0 |
| 50 kHz | 50,000.0 Hz | 20,195 | 3,917,826 | 0.9795% | 0 |
| 19 us period | 52,631.6 Hz | 21,269 | 4,126,182 | 1.0315% | 0 |
| 100 kHz | 100,000.0 Hz | 40,791 | 7,913,450 | 1.9784% | 0 |

All six workloads matched this overhead to within 0.0001 percentage points at each rate.

**The 1% point is between 50,000 Hz (0.9795%) and 52,631.6 Hz (1.0315%).**

**What that means.** Each tick costs 194 instructions on the workload hart. At that rate they add up to 1% of the hart's retired
instructions, on QEMU, with `SAMPLE_CHECKSUM=0`, and with the E51 asleep.

**What that does not mean.** It is not a 1% slowdown on silicon. Instructions are not time there: CSR reads, MMIO and cache
misses all cost more than an average instruction, and the ISR also evicts the workload's cache lines. QEMU models none of that.
It also says nothing about whether the system works at that rate: mode B shows the E51 drops samples well before it.

### ISR cost

Measured as extra instructions per tick, arithmetic workload, under `-icount`. Identical at 1 kHz and 20 kHz, to the rounding shown.

| ISR contents | Instructions per tick |
| --- | --- |
| Trap entry and exit, timer handling, no sample | 112 |
| Plus sample and ring write, checksum off | 194 |
| Plus checksum | 250 |

The sampling work is therefore 82 instructions, and the checksum 56. The timer handler still does Day 1's gap statistics, so
112 is not the minimum possible.

### Extra misses caused by the ISR

Extra over the telemetry-off run, under `-icount`. All rows are in `docs/data/overhead_summary.txt`. On `load4k`, extra DTLB
read misses were 3.0 per tick at each rate from 100 Hz to 100 kHz.

| Workload | Rate | Extra DTLB read | Extra DTLB write | Extra ITLB |
| --- | --- | --- | --- | --- |
| arith | 1 kHz | 9 | 70 | 0 |
| arith | 20 kHz | 81 | 189 | 0 |
| load4k | 1 kHz | 1,050 | 989 | 9,918 |
| load4k | 20 kHz | 21,081 | 21,020 | 74,690 |
| store4k | 1 kHz | 1,050 | 989 | 9,918 |
| store4k | 20 kHz | 21,081 | 21,020 | 73,991 |

**Inference, not isolated:** these are the ISR's own accesses missing, because a large-stride workload evicted their TLB entries.
Evidence: the count scales with ticks on the workloads that thrash QEMU's TLB and stays tiny on those that do not, and it is
absent with no ISR. No experiment has identified which access misses. ITLB does not scale with ticks in the same way, and on
`load64` at 100 kHz it was below the telemetry-off run (-105). That is not explained.

### E51 drain cost

Measured on real records with every other hart asleep, under `-icount`, at 1 kHz.

| Workload | Instructions per sample, checksum off | With digest (checksum on) | Bytes per sample |
| --- | --- | --- | --- |
| arith | 861.7 | 912.4 | 14.20 |
| branch | 871.3 | 921.8 | 14.40 |
| load64 / store64 | 917.6 | 968.3 | 15.36 |
| load4k / store4k | 969.1 | 1,019.8 | 16.42 |

CRC-32: 20.01 instructions per byte. COBS: 13.04 instructions per byte (1,024-byte buffer). So CRC plus COBS are a large
share of drain cost: at 14.20 bytes per sample they come to about 470 of the 862 instructions (calculated from these numbers).

### Live drain (mode B)

Four U54s tick and work (arith, branch, load4k, store4k), the E51 drains, 4096-slot rings. Instruction counts here are global, so
this table reports rates and rings only. Under `-icount`.

| Requested rate | Achieved mean rate, per hart | Tick gap range, all harts (us) | Max ring occupancy | Drop rate per hart |
| --- | --- | --- | --- | --- |
| 100 Hz | 100.0-100.1 Hz | 8,000-12,000 | 17 | 0 |
| 1 kHz | 1,000.0-1,000.2 Hz | 789-1,211 | 32 | 0 |
| 5 kHz | 5,000.0-5,000.2 Hz | 88-312 | 96 | 0 |
| 10 kHz | 10,000.0-10,000.2 Hz | 0-232 | 160 | 0 |
| 20 kHz | 19,999.9-20,000.2 Hz | 0-317 | 320 | 0 |
| 50 kHz | 49,999.9-50,000.1 Hz | 0-1,095 | 4,096 (full) | 16.99-19.20% |
| 100 kHz | 100,000.0-100,000.2 Hz | 0-1,750 | 4,096 (full) | 53.59-58.41% |

The first drops appear somewhere between 20 and 50 kHz. No rate between those was run in this mode. The mean rate stays on target
because deadlines are scheduled, not measured from "now". The individual gaps do not, as Day 2 explained. A gap of 0 us means two
ticks arrived within the same microsecond.

## What could go wrong

- **Baseline drift.** `bench/baseline.json` holds numbers from this QEMU and compiler. A different version may need a new baseline
  from `make gate-baseline`, and accepting one should be a reviewed change.
- **The gate sees only these six loops.** Firmware changes outside them pass.
- **Reading mode B's `runtime_us` as workload cost.** It is global virtual time, shared with every other active hart.
