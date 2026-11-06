#!/usr/bin/env python3
"""CI gate: compare fixed-work counters with a stored baseline.

Measured under -icount, on one U54, other harts asleep, sampling off. Repeat runs are identical,
so there is no variance to test: this is a plain threshold, not a statistical test. There is no
IPC metric, because IPC is flat on QEMU.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

LINE = re.compile(r"^BENCH wl=(\S+) (.*)$", re.MULTILINE)
GATED = ("minstret", "dtlb_read_pki", "dtlb_write_pki")
ZERO_FLOOR = 0.001      # a metric whose baseline is 0 fails above this, as no percentage exists


@dataclass(frozen=True)
class Check:
    workload: str
    metric: str
    baseline: float
    current: float
    percent: float | None
    failed: bool


def parse_results(text: str) -> dict[str, dict[str, float]]:
    """Gate metrics per workload, from the BENCH lines in a console log."""
    results: dict[str, dict[str, float]] = {}
    for name, fields in LINE.findall(text):
        values = {k: int(v) for k, v in (pair.split("=") for pair in fields.split())}
        instructions = values["minstret"]
        results[name] = {
            "minstret": float(instructions),
            "dtlb_read_pki": 1000.0 * values["dtlb_read"] / instructions,
            "dtlb_write_pki": 1000.0 * values["dtlb_write"] / instructions,
        }
    return results


def check_metric(workload: str, metric: str, baseline: float, current: float, threshold: float) -> Check:
    """Only a rise fails. An improvement is reported but does not fail."""
    if baseline == 0:
        return Check(workload, metric, baseline, current, None, current > ZERO_FLOOR)
    percent = 100.0 * (current - baseline) / baseline
    return Check(workload, metric, baseline, current, percent, percent > threshold)


def compare(baseline: dict, current: dict, threshold: float) -> tuple[list[Check], list[str]]:
    """Every check, plus problems that make the comparison itself invalid."""
    problems = [f"workload {w} missing from this run" for w in baseline if w not in current]
    problems += [f"workload {w} has no baseline" for w in current if w not in baseline]
    checks = [check_metric(w, m, baseline[w][m], current[w][m], threshold)
              for w in baseline if w in current for m in GATED]
    return checks, problems


def report(checks: list[Check], threshold: float) -> None:
    print(f"workload  metric           baseline         current          change   (threshold +{threshold}%)")
    for c in checks:
        change = "n/a" if c.percent is None else f"{c.percent:+.3f}%"
        print(f"{c.workload:9} {c.metric:15} {c.baseline:15.4f}  {c.current:15.4f}  {change:>9}  "
              f"{'FAIL' if c.failed else 'ok'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("--threshold", type=float, default=3.0, help="percent rise that fails")
    parser.add_argument("--write-baseline", action="store_true")
    args = parser.parse_args()

    current = parse_results(args.log.read_text(errors="replace"))
    if not current:
        print("GATE FAIL: no BENCH lines in", args.log)
        return 1
    if args.write_baseline:
        args.baseline.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
        print(f"wrote {args.baseline} with {len(current)} workloads")
        return 0

    checks, problems = compare(json.loads(args.baseline.read_text()), current, args.threshold)
    report(checks, args.threshold)
    for problem in problems:
        print("PROBLEM:", problem)
    failed = [c for c in checks if c.failed]
    if failed or problems:
        print(f"GATE FAIL: {len(failed)} metric(s) over threshold, {len(problems)} problem(s)")
        return 1
    print(f"GATE PASS: {len(checks)} metrics within +{args.threshold}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
