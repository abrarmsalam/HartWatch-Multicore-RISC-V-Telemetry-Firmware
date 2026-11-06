# hpm_backend -- reading performance counters

## What it does

It is the one doorway to the performance counters. The sampler calls `hpm_snapshot()` and gets back `mcycle`,
`minstret` and three event counters. It never touches a CSR itself. `hpm_init()` chooses which events the three
counters count, and each sampled hart calls it for itself because CSRs are per-hart.

| File | Used for | State |
| --- | --- | --- |
| `fw/src/hpm/hpm_qemu.c` | QEMU `sifive_u` (default) | Tested by `make test` |
| `fw/src/hpm/hpm_sifive.c` | FU540 silicon | Compiles. **Untested on hardware** |
| `fw/tests/hpm_fake.c` | Host unit tests | Returns predictable values |

To build another backend: `make HPM_BACKEND=sifive BUILD=build/sifive`.

## Why this design

Counters behave differently on each target. On QEMU, `mcycle` and `minstret` are the same clock. On silicon they
should be separate. The event numbers differ too. Hiding that behind one small interface means the rest of the
firmware does not change between targets, and a fake backend lets the sampler run on a laptop.

The choice is made at build time, not with a function pointer, because the call runs in the ISR on the hart being
measured, and that hart should do as little extra work as possible.

## What could go wrong

- **Reading `minstret` as "this hart's instructions" on QEMU.** Under `-icount` it is global virtual time. It matches
  the hart's real count only when every other hart is asleep. See `docs/DESIGN.md`.
- **Reading the TLB events as hardware behaviour.** They count fills of QEMU's software TLB.
- **Changing a selector without writing 0 first.** QEMU keeps the old event counting too, which ruined one probe run.
- **Trusting `hpm_sifive`.** Its selector values and its count of two event counters are from memory and need checking
  against the FU540 manual. It has no separate DTLB write-miss event in that memory, so slot 1 stays 0.
