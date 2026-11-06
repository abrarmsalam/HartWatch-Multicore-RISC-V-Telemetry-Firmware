"""Mann-Whitney U and the verdict rule. Expected values worked out by hand from the definition."""
from __future__ import annotations

import pytest

import wallclock as w


def test_complete_separation() -> None:
    u, p = w.mann_whitney_u([1, 2, 3, 4, 5], [6, 7, 8, 9, 10])
    assert u == 0
    assert p == pytest.approx(0.01219, abs=1e-4)    # z = 12 / sqrt(275/12) = 2.5067


def test_u_is_the_count_of_pairs_where_a_beats_b() -> None:
    a, b = [3.0, 5.0], [1.0, 4.0, 6.0]
    wins = sum(x > y for x in a for y in b)
    assert w.mann_whitney_u(a, b)[0] == wins


def test_ties_count_half() -> None:
    assert w.mann_whitney_u([1, 2], [2, 3])[0] == 0.5


def test_identical_samples_give_no_signal() -> None:
    assert w.mann_whitney_u([5.0] * 10, [5.0] * 10)[1] == 1.0


def test_symmetry() -> None:
    a, b = [1.0, 2.5, 9.0, 4.0], [3.0, 7.0, 8.0, 6.0]
    ua, pa = w.mann_whitney_u(a, b)
    ub, pb = w.mann_whitney_u(b, a)
    assert ua + ub == 16
    assert pa == pytest.approx(pb)


def test_fails_only_when_significant_and_large() -> None:
    low = [1.00 + i * 0.001 for i in range(20)]
    assert w.judge(low, [v * 1.10 for v in low], 0.05, 3).failed
    assert not w.judge(low, [v + 0.02 for v in low], 0.05, 3).failed           # significant, under 3%
    assert not w.judge([v * 1.10 for v in low], low, 0.05, 3).failed           # faster never fails
    spread = [float(i) for i in range(1, 21)]
    assert not w.judge(spread, [v * 1.10 for v in spread], 0.05, 3).failed     # big but not significant
