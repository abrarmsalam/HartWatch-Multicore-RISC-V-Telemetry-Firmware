#!/usr/bin/env python3
"""Run the overhead sweep (docs/benchmarks.md). Inside the toolchain container, one run at a time.

Part A: one U54 runs fixed work, E51 asleep: exact instruction overhead on the workload hart.
Part B: four U54s tick and work, E51 drains live: ring occupancy, drops, achieved tick rate.
Writes CSV files to docs/data/. All runs are under -icount, and SAMPLE_CHECKSUM is off unless a
row says otherwise.
"""
from __future__ import annotations

import csv
import re
import subprocess
import sys
from pathlib import Path

WORKLOADS = ("arith", "branch", "load64", "load4k", "store64", "store4k")
RATES = (100, 1000, 5000, 10000, 20000, 50000, 100000)
SCALE = 10
OUT = Path("docs/data")


def fields(line: str) -> dict[str, str]:
    return dict(pair.split("=", 1) for pair in line.split()[1:])


def run(build: str, checksum: int, sampler: int, defs: str) -> str:
    cmd = ["make", "-s", "bench", f"BUILD={build}", f"SAMPLE_CHECKSUM={checksum}", f"BENCH_SAMPLER={sampler}",
           f"BENCH={defs}"]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError(f"{defs}: {done.stdout[-400:]} {done.stderr[-400:]}")
    return done.stdout


def part_a(wl: int, hz: int, checksum: int = 0, sampler: int = 1) -> dict[str, str]:
    defs = f"-DBENCH_MODE=1 -DBENCH_WL={wl} -DBENCH_HZ={hz} -DBENCH_SCALE={SCALE}"
    text = run(f"build/sw{checksum}", checksum, sampler, defs)
    row = {"workload": WORKLOADS[wl], "hz": str(hz), "checksum": str(checksum), "sampling": str(sampler)}
    for line in text.splitlines():
        if line.startswith("BENCH wl="):
            row.update(fields(line))
        elif line.startswith(("TICKS", "DRAIN", "CRC", "COBS")):
            tag = line.split()[0].lower()
            row.update({f"{tag}_{k}": v for k, v in fields(line).items()})
    return row


def part_b(hz: int) -> list[dict[str, str]]:
    defs = f"-DBENCH_MODE=2 -DBENCH_HZ={hz} -DBENCH_RING_SLOTS=4096 -DBENCH_SCALE={SCALE // 2}"
    text = run("build/sw0", 0, 1, defs)
    wire = next(fields(l) for l in text.splitlines() if l.startswith("WIRE"))
    return [{"hz": str(hz), **fields(l), "wire_bytes": wire["bytes"]} for l in text.splitlines() if l.startswith("HART")]


def write(name: str, rows: list[dict[str, str]]) -> None:
    keys: list[str] = []
    for row in rows:
        keys += [k for k in row if k not in keys]
    with (OUT / name).open("w", newline="") as f:
        writer = csv.DictWriter(f, keys, restval="")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    rows = []
    for wl, name in enumerate(WORKLOADS):
        rows.append(part_a(wl, 0))
        print(name, "off", rows[-1]["minstret"], flush=True)
        for hz in RATES:
            rows.append(part_a(wl, hz))
            print(name, hz, rows[-1]["minstret"], flush=True)
    write("overhead_a.csv", rows)

    isr = []
    for hz in (1000, 20000):
        isr.append(part_a(0, hz, checksum=0, sampler=0))
        isr.append(part_a(0, hz, checksum=0, sampler=1))
        isr.append(part_a(0, hz, checksum=1, sampler=1))
        print("isr", hz, flush=True)
    isr.append(part_a(0, 0, checksum=1, sampler=1))
    for wl in range(len(WORKLOADS)):        # drain cost with the digest on, per workload
        isr.append(part_a(wl, 1000, checksum=1, sampler=1))
    write("isr_cost.csv", isr)

    repeat = [part_a(0, 1000), part_a(3, 20000)]
    write("overhead_repeat.csv", repeat)

    b = []
    for hz in RATES:
        b += part_b(hz)
        print("B", hz, [r["dropped"] for r in b[-4:]], flush=True)
    write("overhead_b.csv", b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
