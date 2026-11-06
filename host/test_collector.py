"""Collector logic with no database. test_db.py covers the real thing."""
from __future__ import annotations

import pytest

import collector as c
import decoder as d
from symbols import Location


class FakeSymbols:
    def lookup(self, pc: int) -> Location:
        return Location(f"f{pc:x}", "x.c", 7)


def sample(seq: int = 1, mtime: int = 2_500_000, mepc: int = 0x80) -> d.Sample:
    return d.Sample(2, seq, mtime, 10, 20, 30, 40, 50, mepc)


def test_row_has_schema_order_and_maps_mtime_to_ts() -> None:
    row = c.sample_row(9, sample(), FakeSymbols())
    assert row[0].isoformat() == "1970-01-01T00:00:02.500000+00:00"
    assert row[1:] == (9, 2, 1, 2_500_000, 10, 20, 30, 40, 50, 0x80, "f80", "x.c", 7)


def test_value_too_large_for_bigint_is_refused_not_wrapped() -> None:
    with pytest.raises(ValueError):
        c.sample_row(1, sample(mepc=1 << 63), FakeSymbols())


def test_batches_split_and_keep_remainder() -> None:
    assert list(c.batches(range(5), 2)) == [[0, 1], [2, 3], [4]]
    assert list(c.batches([], 2)) == []


def end(digest: int, has_digest: bool = True) -> d.EndFrame:
    return d.EndFrame(1, 3, 0, 1, 1, 0, has_digest, digest)


def test_digest_check() -> None:
    good = d.fold_digest(0, sample())
    assert c.digest_ok(d.HartStream(samples=[sample()], end=end(good))) is True
    assert c.digest_ok(d.HartStream(samples=[sample()], end=end(good ^ 1))) is False
    assert c.digest_ok(d.HartStream(samples=[sample()], end=end(0, False))) is None
    assert c.digest_ok(d.HartStream(samples=[sample()])) is None


def test_hart_row_without_end_frame_has_no_attempts() -> None:
    assert c.hart_row(1, 2, d.HartStream(frames=4, wire_bytes=99, dropped=3))[3] is None
