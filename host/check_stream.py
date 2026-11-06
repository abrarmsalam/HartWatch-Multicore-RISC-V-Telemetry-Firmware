#!/usr/bin/env python3
"""Validate a captured UART1 stream against the firmware's own END-frame totals."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import decoder as d


def check_hart(hart: int, s: d.HartStream, expect_attempts: int | None, allow_drops: bool) -> list[str]:
    """Return every problem found for one hart. Empty means it passed."""
    problems: list[str] = []
    end = s.end
    if end is None:
        return [f"hart {hart}: no END frame"]
    if s.frame_seqs != list(range(len(s.frame_seqs))):
        problems.append(f"hart {hart}: frame_seq has gaps, frames were lost")
    if len(s.samples) != end.drained:
        problems.append(f"hart {hart}: decoded {len(s.samples)} samples, E51 sent {end.drained}")
    if end.drained + end.dropped != end.attempts:
        problems.append(f"hart {hart}: drained + dropped != attempts")
    if expect_attempts is not None and end.attempts != expect_attempts:
        problems.append(f"hart {hart}: {end.attempts} attempts, expected {expect_attempts}")
    if end.dropped and not allow_drops:
        problems.append(f"hart {hart}: {end.dropped} drops")
    if allow_drops and not end.dropped:
        problems.append(f"hart {hart}: expected drops, saw none")
    if end.bad:
        problems.append(f"hart {hart}: firmware rejected {end.bad} records")

    gaps = 0
    for prev, cur in zip(s.samples, s.samples[1:]):
        skipped = d.seq_gap(prev.seq, cur.seq)
        if skipped is None:
            problems.append(f"hart {hart}: seq went backwards at {cur.seq}")
        else:
            gaps += skipped
        if cur.mtime < prev.mtime or cur.mcycle < prev.mcycle or cur.minstret < prev.minstret:
            problems.append(f"hart {hart}: a counter went backwards at seq {cur.seq}")
    if s.samples:
        gaps += s.samples[0].seq + ((end.attempts - 1 - s.samples[-1].seq) & d.SEQ_MASK)
    if gaps != end.dropped:
        problems.append(f"hart {hart}: seq gaps total {gaps}, drop counter says {end.dropped}")

    if end.has_digest:
        digest = 0
        for sample in s.samples:
            digest = d.fold_digest(digest, sample)
        if digest != end.digest:
            problems.append(f"hart {hart}: digest mismatch: decoded data differs from the raw records")
    return problems


def report(stream: d.Stream) -> None:
    print(" hart  samples  dropped  frames  wire_bytes  bytes/sample  span_us  bytes/s")
    total_samples = 0
    first = last = None
    for hart in sorted(stream.harts):
        s = stream.harts[hart]
        n = len(s.samples)
        total_samples += n
        if n:
            first = s.samples[0].mtime if first is None else min(first, s.samples[0].mtime)
            last = s.samples[-1].mtime if last is None else max(last, s.samples[-1].mtime)
        span = s.samples[-1].mtime - s.samples[0].mtime if n > 1 else 0
        rate = s.wire_bytes * 1_000_000 / span if span else 0.0
        print(f" {hart:4}  {n:7}  {s.dropped:7}  {s.frames:6}  {s.wire_bytes:10}  "
              f"{s.wire_bytes / max(n, 1):12.2f}  {span:7}  {rate:7.0f}")
    span = (last - first) if first is not None and last is not None else 0
    rate = stream.total_bytes * 1_000_000 / span if span else 0.0
    print(f" all   {total_samples:7}  bytes {stream.total_bytes}  "
          f"bytes/sample {stream.total_bytes / max(total_samples, 1):.2f}  span_us {span}  bytes/s {rate:.0f}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--attempts", type=int, help="expected samples attempted per hart")
    parser.add_argument("--harts", type=int, default=4)
    parser.add_argument("--allow-drops", action="store_true", help="this run is meant to drop samples")
    args = parser.parse_args()

    stream = d.decode_stream(args.capture.read_bytes())
    report(stream)

    problems = [f"{stream.bad_frames} damaged frames"] if stream.bad_frames else []
    if len(stream.harts) != args.harts:
        problems.append(f"saw {len(stream.harts)} harts, expected {args.harts}")
    for hart, s in sorted(stream.harts.items()):
        problems += check_hart(hart, s, args.attempts, args.allow_drops)

    for problem in problems[:20]:
        print("PROBLEM:", problem)
    print("DECODER FAIL" if problems else "DECODER PASS")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
