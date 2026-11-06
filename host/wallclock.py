#!/usr/bin/env python3
"""Wall-clock comparison of two firmware images. THIS MEASURES QEMU HOST RUNTIME, NOT TARGET
PERFORMANCE: it depends on the host machine, its load and how QEMU translates the code.

Runs without -icount, so results vary between runs. That variance is why this job uses
Mann-Whitney U, and the -icount gate does not. Fails only if the difference is statistically
significant AND the median rises by more than the threshold.
"""
from __future__ import annotations

import argparse
import math
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

QEMU = ("qemu-system-riscv64 -machine sifive_u -smp 5 -m 256M -nographic -bios none -no-reboot "
        "-serial mon:stdio -serial null -kernel").split()


@dataclass(frozen=True)
class Verdict:
    u: float
    p: float
    baseline_median: float
    candidate_median: float
    percent: float
    failed: bool


def mann_whitney_u(a: list[float], b: list[float]) -> tuple[float, float]:
    """(U for a, two-sided p). Normal approximation with tie and continuity corrections, which
    needs about 8 or more values per side to be reasonable."""
    n1, n2 = len(a), len(b)
    combined = sorted((value, side) for side, values in enumerate((a, b)) for value in values)
    ranks: dict[float, float] = {}
    tie_term = 0.0
    i = 0
    while i < len(combined):
        j = i
        while j < len(combined) and combined[j][0] == combined[i][0]:
            j += 1
        ranks[combined[i][0]] = (i + j + 1) / 2
        count = j - i
        tie_term += count ** 3 - count
        i = j
    u = sum(ranks[v] for v in a) - n1 * (n1 + 1) / 2
    n = n1 + n2
    variance = n1 * n2 / 12 * ((n + 1) - tie_term / (n * (n - 1)))
    if variance == 0:
        return u, 1.0
    mean = n1 * n2 / 2
    z = max(abs(u - mean) - 0.5, 0.0) / math.sqrt(variance)
    return u, math.erfc(z / math.sqrt(2))


def judge(baseline: list[float], candidate: list[float], alpha: float, threshold: float) -> Verdict:
    u, p = mann_whitney_u(baseline, candidate)
    base, cand = statistics.median(baseline), statistics.median(candidate)
    percent = 100.0 * (cand - base) / base
    return Verdict(u, p, base, cand, percent, p < alpha and percent > threshold)


def time_run(elf: Path) -> float:
    start = time.perf_counter()
    subprocess.run([*QEMU, str(elf)], check=True, stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    return time.perf_counter() - start


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--runs", type=int, default=20)
    parser.add_argument("--alpha", type=float, default=0.05)
    parser.add_argument("--threshold", type=float, default=3.0)
    args = parser.parse_args()

    base: list[float] = []
    cand: list[float] = []
    for _ in range(args.runs):      # interleaved, so slow drift in the host hits both sides
        base.append(time_run(args.baseline))
        cand.append(time_run(args.candidate))
    v = judge(base, cand, args.alpha, args.threshold)

    print("QEMU HOST RUNTIME, not target performance. No -icount.")
    print(f"runs per side {args.runs}: baseline median {v.baseline_median:.4f} s "
          f"(min {min(base):.4f}, max {max(base):.4f}), candidate median {v.candidate_median:.4f} s "
          f"(min {min(cand):.4f}, max {max(cand):.4f})")
    print(f"median change {v.percent:+.2f}%, Mann-Whitney U {v.u}, p {v.p:.6f}")
    print("WALLCLOCK FAIL" if v.failed else "WALLCLOCK PASS")
    return 1 if v.failed else 0


if __name__ == "__main__":
    sys.exit(main())
