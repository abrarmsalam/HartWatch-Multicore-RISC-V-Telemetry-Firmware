"""Gate logic, including the exact threshold boundary."""
from __future__ import annotations

import gate as g

LOG = """noise
BENCH wl=arith minstret=1000 mcycle=1002 mtime_us=1 dtlb_read=0 dtlb_write=0 itlb=0
BENCH wl=load64 minstret=2000 mcycle=2002 mtime_us=2 dtlb_read=10 dtlb_write=4 itlb=1
BENCH DONE
"""


def test_parse_computes_misses_per_thousand_instructions() -> None:
    r = g.parse_results(LOG)
    assert r["load64"] == {"minstret": 2000.0, "dtlb_read_pki": 5.0, "dtlb_write_pki": 2.0}
    assert r["arith"]["dtlb_read_pki"] == 0.0


def test_exactly_at_threshold_passes_and_just_above_fails() -> None:
    assert not g.check_metric("w", "minstret", 1000, 1030, 3).failed
    assert g.check_metric("w", "minstret", 1000, 1031, 3).failed


def test_threshold_is_configurable() -> None:
    assert not g.check_metric("w", "minstret", 1000, 1040, 5).failed
    assert g.check_metric("w", "minstret", 1000, 1040, 1).failed


def test_improvement_never_fails() -> None:
    c = g.check_metric("w", "minstret", 1000, 500, 3)
    assert c.percent == -50 and not c.failed


def test_zero_baseline_fails_only_when_the_metric_appears() -> None:
    assert not g.check_metric("w", "dtlb_read_pki", 0, 0, 3).failed
    c = g.check_metric("w", "dtlb_read_pki", 0, 0.5, 3)
    assert c.failed and c.percent is None


def test_missing_workload_invalidates_the_comparison() -> None:
    base = g.parse_results(LOG)
    _, problems = g.compare(base, {"arith": base["arith"]}, 3)
    assert problems == ["workload load64 missing from this run"]
    _, problems = g.compare({"arith": base["arith"]}, base, 3)
    assert problems == ["workload load64 has no baseline"]


def test_all_metrics_are_checked() -> None:
    base = g.parse_results(LOG)
    checks, _ = g.compare(base, base, 3)
    assert len(checks) == 2 * len(g.GATED)
    assert not any(c.failed for c in checks)
