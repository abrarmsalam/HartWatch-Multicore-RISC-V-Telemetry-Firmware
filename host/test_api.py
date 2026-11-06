"""API tests with a fake store, so they need no database. test_db.py covers the real SQL."""
from __future__ import annotations

from typing import Optional

import pytest
from fastapi.testclient import TestClient

import api


def run(run_id: int) -> api.RunDetail:
    hart = api.HartTotals(hart=1, samples=10, attempts=12, dropped=2, frames=1, wire_bytes=99, digest_ok=True)
    return api.RunDetail(run_id=run_id, name=f"r{run_id}", git_commit=None, timing_mode="icount", sample_hz=1000,
                         wire_bytes=99, bad_frames=0, samples=10, dropped=2, harts=[hart])


class FakeStore:
    def __init__(self) -> None:
        self.series_args: tuple | None = None

    def runs(self) -> list[api.Run]:
        return [run(2), run(1)]

    def run(self, run_id: int) -> Optional[api.RunDetail]:
        return run(run_id) if run_id in (1, 2) else None

    def series(self, run_id: int, hart: int, column: str, limit: int) -> list[api.SeriesPoint]:
        self.series_args = (run_id, hart, column, limit)
        return [api.SeriesPoint(mtime_us=1000, delta=5)]

    def hot_functions(self, run_id: int) -> list[api.HotFunction]:
        share = 90.0 if run_id == 1 else 60.0
        return [api.HotFunction(hart=1, symbol="wl_alu", samples=9, percent=share),
                api.HotFunction(hart=1, symbol=f"only_in_{run_id}", samples=1, percent=100 - share)]


@pytest.fixture()
def store() -> FakeStore:
    return FakeStore()


@pytest.fixture()
def client(store: FakeStore):
    api.app.dependency_overrides[api.get_store] = lambda: store
    yield TestClient(api.app)
    api.app.dependency_overrides.clear()


def test_list_runs(client: TestClient) -> None:
    body = client.get("/runs").json()
    assert [r["run_id"] for r in body] == [2, 1]


def test_run_detail_has_harts(client: TestClient) -> None:
    body = client.get("/runs/1").json()
    assert body["harts"][0] == {"hart": 1, "samples": 10, "attempts": 12, "dropped": 2, "frames": 1,
                                "wire_bytes": 99, "digest_ok": True}


@pytest.mark.parametrize("path", ["/runs/9", "/runs/9/hot-functions", "/runs/9/harts/1/series",
                                  "/runs/9/diff/1", "/runs/1/diff/9", "/runs/1/harts/7/series"])
def test_unknown_things_are_404(client: TestClient, path: str) -> None:
    assert client.get(path).status_code == 404


def test_series_maps_the_metric_to_a_column(client: TestClient, store: FakeStore) -> None:
    assert client.get("/runs/1/harts/1/series?metric=dtlb_write&limit=7").json() == [{"mtime_us": 1000, "delta": 5}]
    assert store.series_args == (1, 1, "hpm1", 7)


def test_unknown_metric_never_reaches_the_store(client: TestClient, store: FakeStore) -> None:
    response = client.get("/runs/1/harts/1/series", params={"metric": "hpm0; DROP TABLE samples"})
    assert response.status_code == 422
    assert store.series_args is None


@pytest.mark.parametrize("limit", [0, 20001])
def test_limit_is_bounded(client: TestClient, limit: int) -> None:
    assert client.get(f"/runs/1/harts/1/series?limit={limit}").status_code == 422


def test_hot_functions(client: TestClient) -> None:
    assert client.get("/runs/1/hot-functions").json()[0]["symbol"] == "wl_alu"


def test_diff_covers_functions_on_either_side(client: TestClient) -> None:
    rows = {r["symbol"]: r for r in client.get("/runs/2/diff/1").json()}
    assert rows["wl_alu"]["change_points"] == -30.0
    assert rows["only_in_1"]["run_percent"] == 0.0
    assert rows["only_in_2"]["baseline_percent"] == 0.0


def test_diff_puts_the_biggest_change_first() -> None:
    def f(symbol: str, percent: float) -> api.HotFunction:
        return api.HotFunction(hart=1, symbol=symbol, samples=1, percent=percent)

    rows = api.diff_hot([f("a", 50), f("b", 50)], [f("a", 49), f("b", 10)])
    assert [r.symbol for r in rows] == ["b", "a"]


def test_openapi_lists_every_endpoint(client: TestClient) -> None:
    paths = client.get("/openapi.json").json()["paths"]
    assert set(paths) == {"/runs", "/runs/{run_id}", "/runs/{run_id}/harts/{hart}/series",
                          "/runs/{run_id}/hot-functions", "/runs/{run_id}/diff/{baseline_id}"}


def test_dashboard_is_served(client: TestClient) -> None:
    response = client.get("/")
    assert response.status_code == 200
    assert "<title>HartWatch</title>" in response.text
