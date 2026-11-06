"""REST API over the HartWatch database. OpenAPI docs are served at /docs and /openapi.json.

Everything here reports QEMU runs. Hot-function shares are estimates from sampling.
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Iterator, Optional, Protocol

from fastapi import Depends, FastAPI, HTTPException, Query
from fastapi.responses import HTMLResponse
from pydantic import BaseModel

STATIC = Path(__file__).parent / "static"
SQL = Path(__file__).parent / "sql"

# The only columns a caller can ask for. The value goes into SQL text, so it must never come
# from the request directly.
METRICS = {"dtlb_read": "hpm0", "dtlb_write": "hpm1", "itlb": "hpm2", "mcycle": "mcycle", "minstret": "minstret"}


class Run(BaseModel):
    run_id: int
    name: str
    git_commit: Optional[str]
    timing_mode: str
    sample_hz: int
    wire_bytes: int
    bad_frames: int
    samples: int
    dropped: int


class HartTotals(BaseModel):
    hart: int
    samples: int
    attempts: Optional[int]
    dropped: int
    frames: int
    wire_bytes: int
    digest_ok: Optional[bool]


class RunDetail(Run):
    harts: list[HartTotals]


class SeriesPoint(BaseModel):
    mtime_us: int
    delta: int


class HotFunction(BaseModel):
    hart: int
    symbol: str
    samples: int
    percent: float


class DiffRow(BaseModel):
    hart: int
    symbol: str
    baseline_percent: float
    run_percent: float
    change_points: float


class Store(Protocol):
    def runs(self) -> list[Run]: ...
    def run(self, run_id: int) -> Optional[RunDetail]: ...
    def series(self, run_id: int, hart: int, column: str, limit: int) -> list[SeriesPoint]: ...
    def hot_functions(self, run_id: int) -> list[HotFunction]: ...


def diff_hot(baseline: list[HotFunction], current: list[HotFunction]) -> list[DiffRow]:
    """Change in each function's share of samples, biggest change first. A function present on
    one side only counts as 0% on the other."""
    before = {(f.hart, f.symbol): f.percent for f in baseline}
    after = {(f.hart, f.symbol): f.percent for f in current}
    rows = [DiffRow(hart=hart, symbol=symbol, baseline_percent=before.get((hart, symbol), 0.0),
                    run_percent=after.get((hart, symbol), 0.0),
                    change_points=round(after.get((hart, symbol), 0.0) - before.get((hart, symbol), 0.0), 2))
            for hart, symbol in sorted(before.keys() | after.keys())]
    return sorted(rows, key=lambda r: (r.hart, -abs(r.change_points)))


class PostgresStore:
    """One short-lived connection per request: simple, and enough for a local tool."""

    def __init__(self, url: str) -> None:
        self.url = url

    def _fetch(self, sql: str, args: dict) -> list[tuple]:
        import psycopg2

        conn = psycopg2.connect(self.url)
        try:
            with conn.cursor() as cur:
                cur.execute(sql, args)
                return cur.fetchall()
        finally:
            conn.close()

    RUN_SQL = """
        SELECT r.run_id, r.name, r.git_commit, r.timing_mode, r.sample_hz, r.wire_bytes, r.bad_frames,
               coalesce(sum(h.samples), 0)::bigint, coalesce(sum(h.dropped), 0)::bigint
        FROM runs r LEFT JOIN run_harts h USING (run_id) {where}
        GROUP BY r.run_id ORDER BY r.run_id DESC"""

    @staticmethod
    def _run(row: tuple) -> dict:
        names = ("run_id", "name", "git_commit", "timing_mode", "sample_hz", "wire_bytes", "bad_frames",
                 "samples", "dropped")
        return dict(zip(names, row))

    def runs(self) -> list[Run]:
        return [Run(**self._run(r)) for r in self._fetch(self.RUN_SQL.format(where=""), {})]

    def run(self, run_id: int) -> Optional[RunDetail]:
        rows = self._fetch(self.RUN_SQL.format(where="WHERE r.run_id = %(id)s"), {"id": run_id})
        if not rows:
            return None
        harts = self._fetch("SELECT hart, samples, attempts, dropped, frames, wire_bytes, digest_ok"
                            " FROM run_harts WHERE run_id = %(id)s ORDER BY hart", {"id": run_id})
        names = ("hart", "samples", "attempts", "dropped", "frames", "wire_bytes", "digest_ok")
        return RunDetail(**self._run(rows[0]), harts=[HartTotals(**dict(zip(names, h))) for h in harts])

    def series(self, run_id: int, hart: int, column: str, limit: int) -> list[SeriesPoint]:
        sql = (f"SELECT mtime, delta FROM (SELECT mtime, {column} - lag({column}) OVER (ORDER BY seq) AS delta"
               " FROM samples WHERE run_id = %(id)s AND hart = %(hart)s) s"
               " WHERE delta IS NOT NULL ORDER BY mtime LIMIT %(limit)s")
        rows = self._fetch(sql, {"id": run_id, "hart": hart, "limit": limit})
        return [SeriesPoint(mtime_us=m, delta=d) for m, d in rows]

    def hot_functions(self, run_id: int) -> list[HotFunction]:
        rows = self._fetch((SQL / "hot_functions.sql").read_text(), {"run_id": run_id})
        return [HotFunction(hart=h, symbol=s, samples=int(n), percent=float(p)) for h, s, n, p in rows]


def get_store() -> Store:
    url = os.environ.get("HW_DB_URL")
    if not url:
        raise HTTPException(503, "HW_DB_URL is not set")
    return PostgresStore(url)


app = FastAPI(title="HartWatch", version="0.1.0",
              description="Telemetry from QEMU runs. These numbers describe QEMU, not hardware.")


def require_run(store: Store, run_id: int) -> RunDetail:
    run = store.run(run_id)
    if run is None:
        raise HTTPException(404, f"run {run_id} not found")
    return run


@app.get("/", response_class=HTMLResponse, include_in_schema=False)
def dashboard() -> str:
    return (STATIC / "index.html").read_text()


@app.get("/runs", response_model=list[Run])
def list_runs(store: Store = Depends(get_store)) -> list[Run]:
    return store.runs()


@app.get("/runs/{run_id}", response_model=RunDetail)
def get_run(run_id: int, store: Store = Depends(get_store)) -> RunDetail:
    return require_run(store, run_id)


@app.get("/runs/{run_id}/harts/{hart}/series", response_model=list[SeriesPoint])
def hart_series(run_id: int, hart: int, metric: str = "dtlb_read", limit: int = Query(2000, ge=1, le=20000),
                store: Store = Depends(get_store)) -> list[SeriesPoint]:
    """Change in a counter between consecutive samples of one hart."""
    if metric not in METRICS:
        raise HTTPException(422, f"metric must be one of {sorted(METRICS)}")
    run = require_run(store, run_id)
    if hart not in {h.hart for h in run.harts}:
        raise HTTPException(404, f"run {run_id} has no hart {hart}")
    return store.series(run_id, hart, METRICS[metric], limit)


@app.get("/runs/{run_id}/hot-functions", response_model=list[HotFunction])
def hot_functions(run_id: int, store: Store = Depends(get_store)) -> list[HotFunction]:
    """Share of timer ticks per function: an estimate from sampling."""
    require_run(store, run_id)
    return store.hot_functions(run_id)


@app.get("/runs/{run_id}/diff/{baseline_id}", response_model=list[DiffRow])
def run_diff(run_id: int, baseline_id: int, store: Store = Depends(get_store)) -> list[DiffRow]:
    """Change in each function's sample share from a baseline run. This is not the CI gate."""
    require_run(store, run_id)
    require_run(store, baseline_id)
    return diff_hot(store.hot_functions(baseline_id), store.hot_functions(run_id))
