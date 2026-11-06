# Counter probe and workloads

## What it does

`make probe` boots a separate firmware image (`fw/probe/probe.c`) that measures what the counters really report. It
is not part of HartWatch. It prints raw numbers and draws no conclusions. Add `QEMU_TIMING=` to run without `-icount`.

`fw/src/workloads.S` holds the loops it runs: arithmetic, branch-heavy, strided load and strided store. The firmware
self-test uses them as well.

## Why this design

The loops are in assembly so the compiler cannot change them. Every path through a loop has the same length, so the true
instruction count is known exactly. That gives `minstret` something to be checked against. Comparing it only between
workloads would have missed that it is not counting instructions at all.

An illegal CSR access is a useful answer here, not a crash, so the probe's trap handler records it and skips the
instruction. Each event selector gets its own counter, and one counter stays on event 0 as a control.

## What could go wrong

- A trapped `csrr` leaves garbage in the result. The probe reports traps per CSR for this reason.
- `make clean` deletes `build/`, so results worth keeping are copied to `docs/data/`.
- The SiFive selector values in the probe are from memory. They read 0 on QEMU, which says nothing about silicon.
