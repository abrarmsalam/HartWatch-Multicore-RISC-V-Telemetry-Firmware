"""Integration tests against a real TimescaleDB. Skipped unless HW_DB_URL is set."""
from __future__ import annotations

import os
from pathlib import Path

import pytest

import collector as c
import decoder as d
from symbols import Symbolizer

URL = os.environ.get("HW_DB_URL")
BUILD = Path(os.environ.get("HW_BUILD", "build"))
pytestmark = pytest.mark.skipif(not URL, reason="HW_DB_URL not set")


@pytest.fixture(scope="module")
def run():
    import psycopg2

    conn = psycopg2.connect(URL)
    c.apply_schema(conn)
    c.apply_schema(conn)        # must be safe to repeat
    stream = d.decode_stream((BUILD / "telemetry.bin").read_bytes())
    info = c.RunInfo("pytest", "icount", 1000, "0" * 64)
    run_id = c.ingest(conn, stream, Symbolizer.from_path(BUILD / "hartwatch.elf"), info)
    yield conn, run_id, stream
    with conn, conn.cursor() as cur:
        cur.execute("DELETE FROM samples WHERE run_id = %s", (run_id,))
        cur.execute("DELETE FROM runs WHERE run_id = %s", (run_id,))
    c.refresh_aggregate(conn)
    conn.close()


def one(conn, sql: str, *args):
    with conn.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchone()


def test_samples_is_a_hypertable(run) -> None:
    conn, _, _ = run
    assert one(conn, "SELECT count(*) FROM timescaledb_information.hypertables"
                     " WHERE hypertable_name = 'samples'")[0] == 1


def test_every_decoded_sample_is_stored(run) -> None:
    conn, run_id, stream = run
    want = sum(len(h.samples) for h in stream.harts.values())
    assert one(conn, "SELECT count(*) FROM samples WHERE run_id = %s", run_id)[0] == want


def test_stored_values_match_the_decoder(run) -> None:
    conn, run_id, stream = run
    s = stream.harts[3].samples[100]
    row = one(conn, "SELECT mtime, mcycle, minstret, hpm0, hpm1, hpm2, mepc FROM samples"
                    " WHERE run_id = %s AND hart = 3 AND seq = %s", run_id, s.seq)
    assert row == (s.mtime, s.mcycle, s.minstret, s.hpm0, s.hpm1, s.hpm2, s.mepc)


def test_run_harts_records_digest_and_totals(run) -> None:
    conn, run_id, stream = run
    assert one(conn, "SELECT count(*), bool_and(digest_ok), sum(wire_bytes) FROM run_harts WHERE run_id = %s",
               run_id)[:2] == (len(stream.harts), True)


def test_aggregate_covers_every_sample(run) -> None:
    conn, run_id, stream = run
    want = sum(len(h.samples) for h in stream.harts.values())
    assert one(conn, "SELECT sum(samples) FROM function_time_1s WHERE run_id = %s", run_id)[0] == want


def test_hot_functions_put_each_workload_first(run) -> None:
    conn, run_id, _ = run
    rows = c.hot_functions(conn, run_id)
    top = {}
    for hart, symbol, _, percent in rows:
        top.setdefault(hart, (symbol, percent))
    assert {h: s for h, (s, _) in top.items()} == {1: "wl_alu", 2: "wl_branch", 3: "wl_stride", 4: "wl_store"}
    assert all(p > 90 for _, p in top.values())


def test_api_store_reads_the_same_data(run) -> None:
    """The SQL in api.py, against the real database."""
    import api

    _, run_id, stream = run
    store = api.PostgresStore(URL)
    detail = store.run(run_id)
    assert detail.samples == sum(len(h.samples) for h in stream.harts.values())
    assert [h.hart for h in detail.harts] == sorted(stream.harts)
    assert run_id in [r.run_id for r in store.runs()]
    assert store.hot_functions(run_id)[0].symbol == "wl_alu"

    samples = stream.harts[3].samples
    series = store.series(run_id, 3, "hpm0", 5)
    assert series[0].delta == samples[1].hpm0 - samples[0].hpm0
    assert series[0].mtime_us == samples[1].mtime

