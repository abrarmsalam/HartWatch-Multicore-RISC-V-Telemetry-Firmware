#!/usr/bin/env python3
"""Turn the sweep CSVs into the tables in docs/benchmarks.md. Prints derived numbers only."""
from __future__ import annotations

import csv
from pathlib import Path

DATA = Path("docs/data")


def load(name: str) -> list[dict[str, str]]:
    with (DATA / name).open() as f:
        return list(csv.DictReader(f))


def n(row: dict[str, str], key: str) -> int:
    return int(row[key]) if row.get(key) else 0


def part_a() -> None:
    rows = load("overhead_a.csv")
    base = {r["workload"]: r for r in rows if r["hz"] == "0"}
    print("wl hz minstret extra overhead% ticks instr/tick achieved_hz maxgap d_rd d_wr d_itlb drops")
    for r in rows:
        if r["hz"] == "0":
            print(f'{r["workload"]} off {n(r,"minstret")} rd={n(r,"dtlb_read")} wr={n(r,"dtlb_write")} itlb={n(r,"itlb")} mtime_us={n(r,"mtime_us")}')
            continue
        b = base[r["workload"]]
        extra = n(r, "minstret") - n(b, "minstret")
        ticks, span = n(r, "ticks_ticks"), n(r, "ticks_span_us")
        achieved = (ticks - 1) * 1e6 / span if span else 0
        print(f'{r["workload"]} {r["hz"]} {n(r,"minstret")} {extra} {100*extra/n(b,"minstret"):.4f}% {ticks} '
              f'{extra/ticks:.1f} {achieved:.1f} {n(r,"ticks_max_gap")} {n(r,"dtlb_read")-n(b,"dtlb_read")} '
              f'{n(r,"dtlb_write")-n(b,"dtlb_write")} {n(r,"itlb")-n(b,"itlb")} {n(r,"ticks_dropped")}')


def isr() -> None:
    rows = load("isr_cost.csv")
    off = {r["workload"]: n(r, "minstret") for r in load("overhead_a.csv") if r["hz"] == "0"}
    off_chk = [n(r, "minstret") for r in rows if r["hz"] == "0"]
    print("ISR: wl hz sampling checksum ticks instr/tick | drain")
    for r in rows:
        if r["hz"] == "0":
            print("checksum build, no timer:", n(r, "minstret"))
            continue
        extra = n(r, "minstret") - off[r["workload"]]
        s = n(r, "drain_samples")
        d = f'drain {n(r,"drain_minstret")/s:.1f} instr/sample, {n(r,"drain_bytes")/s:.2f} B/sample' if s else ""
        print(f'{r["workload"]} {r["hz"]} s={r["sampling"]} c={r["checksum"]} ticks={n(r,"ticks_ticks")} '
              f'{extra/n(r,"ticks_ticks"):.2f} {d}')
    r = rows[0]
    print("CRC instr/byte", n(r, "crc_minstret") / n(r, "crc_bytes"), "COBS instr/byte", n(r, "cobs_minstret") / n(r, "cobs_bytes"))
    a = [r for r in load("overhead_a.csv") if r["hz"] == "1000"]
    for r in a:
        s = n(r, "drain_samples")
        print(f'drain checksum off {r["workload"]}: {n(r,"drain_minstret")/s:.1f} instr/sample, {n(r,"drain_bytes")/s:.2f} B/sample, samples {s}')


def part_b() -> None:
    print("B: hz hart ticks span achieved runtime_us min max attempts dropped drop% max_occ wire")
    for r in load("overhead_b.csv"):
        t, span = n(r, "ticks"), n(r, "span_us")
        print(f'{r["hz"]} h{r["hart"]} {t} {span} {(t-1)*1e6/span if span else 0:.1f} {n(r,"runtime_us")} {n(r,"min_gap")} '
              f'{n(r,"max_gap")} {n(r,"attempts")} {n(r,"dropped")} {100*n(r,"dropped")/max(n(r,"attempts"),1):.2f}% {n(r,"max_occ")} {n(r,"wire_bytes")}')


if __name__ == "__main__":
    part_a()
    isr()
    part_b()
    a = load("overhead_a.csv")
    for r in load("overhead_repeat.csv"):
        m = [x for x in a if x["workload"] == r["workload"] and x["hz"] == r["hz"]][0]
        print("repeat", r["workload"], r["hz"], r["minstret"], m["minstret"], r["minstret"] == m["minstret"])
