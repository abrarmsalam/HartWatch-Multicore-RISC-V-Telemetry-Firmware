#!/usr/bin/env python3
"""Load a UART1 capture into TimescaleDB, with every mepc symbolized."""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Iterable, Iterator, Protocol

import decoder as d
from symbols import Location, Symbolizer

BATCH = 1000
EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)
SQL_DIR = Path(__file__).parent / "sql"
BIGINT_MAX = (1 << 63) - 1


class Lookup(Protocol):
    def lookup(self, pc: int) -> Location: ...


@dataclass(frozen=True)
class RunInfo:
    name: str
    timing_mode: str
    sample_hz: int
    elf_sha256: str
    git_commit: str | None = None


def sample_row(run_id: int, sample: d.Sample, symbols: Lookup) -> tuple:
    """One samples-table row. See the note in schema.sql on what ts means."""
    values = (sample.mtime, sample.mcycle, sample.minstret, sample.hpm0, sample.hpm1, sample.hpm2, sample.mepc)
    if max(values) > BIGINT_MAX:
        raise ValueError(f"hart {sample.hart} seq {sample.seq}: value does not fit a BIGINT column")
    where = symbols.lookup(sample.mepc)
    return (EPOCH + timedelta(microseconds=sample.mtime), run_id, sample.hart, sample.seq, *values,
            where.function, where.file, where.line)


def stream_rows(run_id: int, stream: d.Stream, symbols: Lookup) -> Iterator[tuple]:
    for hart in sorted(stream.harts):
        for sample in stream.harts[hart].samples:
            yield sample_row(run_id, sample, symbols)


def digest_ok(hart: d.HartStream) -> bool | None:
    """None when the build carries no digest, or the END frame never arrived."""
    if hart.end is None or not hart.end.has_digest:
        return None
    digest = 0
    for sample in hart.samples:
        digest = d.fold_digest(digest, sample)
    return digest == hart.end.digest


def hart_row(run_id: int, number: int, hart: d.HartStream) -> tuple:
    attempts = hart.end.attempts if hart.end else None
    return (run_id, number, len(hart.samples), attempts, hart.dropped, hart.frames, hart.wire_bytes, digest_ok(hart))


def batches(rows: Iterable[tuple], size: int = BATCH) -> Iterator[list[tuple]]:
    batch: list[tuple] = []
    for row in rows:
        batch.append(row)
        if len(batch) == size:
            yield batch
            batch = []
    if batch:
        yield batch


def apply_schema(conn) -> None:
    """Continuous aggregates cannot be created inside a transaction, so this uses autocommit."""
    previous = conn.autocommit
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute((SQL_DIR / "schema.sql").read_text())
    conn.autocommit = previous


def refresh_aggregate(conn) -> None:
    previous = conn.autocommit
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute("CALL refresh_continuous_aggregate('function_time_1s', NULL, NULL)")
    conn.autocommit = previous


def ingest(conn, stream: d.Stream, symbols: Lookup, info: RunInfo) -> int:
    """Insert one run in a single transaction, so a failure leaves nothing half-loaded."""
    from psycopg2.extras import execute_values

    with conn, conn.cursor() as cur:
        cur.execute(
            "INSERT INTO runs (name, git_commit, timing_mode, sample_hz, elf_sha256, wire_bytes, bad_frames)"
            " VALUES (%s, %s, %s, %s, %s, %s, %s) RETURNING run_id",
            (info.name, info.git_commit, info.timing_mode, info.sample_hz, info.elf_sha256,
             stream.total_bytes, stream.bad_frames))
        run_id = cur.fetchone()[0]
        execute_values(cur, "INSERT INTO run_harts VALUES %s",
                       [hart_row(run_id, n, h) for n, h in sorted(stream.harts.items())])
        for batch in batches(stream_rows(run_id, stream, symbols)):
            execute_values(cur, "INSERT INTO samples VALUES %s", batch, page_size=BATCH)
    refresh_aggregate(conn)
    return run_id


def hot_functions(conn, run_id: int) -> list[tuple]:
    with conn.cursor() as cur:
        cur.execute((SQL_DIR / "hot_functions.sql").read_text(), {"run_id": run_id})
        return cur.fetchall()


def print_hot_functions(rows: list[tuple]) -> None:
    print(" hart  samples  percent  function")
    for hart, symbol, samples, percent in rows:
        print(f" {hart:4}  {samples:7}  {percent:6}%  {symbol}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--timing", choices=("icount", "host"), required=True)
    parser.add_argument("--hz", type=int, required=True)
    parser.add_argument("--commit")
    parser.add_argument("--db", default=os.environ.get("HW_DB_URL"))
    args = parser.parse_args()
    if not args.db:
        parser.error("set --db or HW_DB_URL")

    import psycopg2

    stream = d.decode_stream(args.capture.read_bytes())
    info = RunInfo(args.name, args.timing, args.hz, hashlib.sha256(args.elf.read_bytes()).hexdigest(), args.commit)
    conn = psycopg2.connect(args.db)
    apply_schema(conn)
    run_id = ingest(conn, stream, Symbolizer.from_path(args.elf), info)
    print(f"run {run_id}: {sum(len(h.samples) for h in stream.harts.values())} samples, "
          f"{stream.bad_frames} damaged frames")
    print_hot_functions(hot_functions(conn, run_id))
    conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
