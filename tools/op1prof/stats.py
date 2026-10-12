"""Paired comparison statistics.

The error bar comes from repeated paired runs, never from sampling counts:
Poisson noise on perf sample counts understated the real run-to-run spread
by about an order of magnitude (same configuration 2.5% apart, the baseline
11% apart, between two capture sets).

With the four to eight pairs a batch can afford, a percentile bootstrap is
too narrow: from four values it can never reach beyond their minimum and
maximum. The interval used is the wider of the bootstrap and Student's t.
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass
from typing import List, Sequence, Tuple

# Two-sided 95% quantiles of Student's t by degrees of freedom; 1.96 beyond.
T975 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262,
        10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145, 15: 2.131, 16: 2.120, 17: 2.110,
        18: 2.101, 19: 2.093, 20: 2.086, 25: 2.060, 30: 2.042, 40: 2.021, 60: 2.000, 120: 1.980}

FASTER, SLOWER, INCONCLUSIVE = "faster", "slower", "inconclusive"
FASTER_SMALL = "faster (below threshold)"
SLOWER_SMALL = "slower (below threshold)"
VERDICTS = (FASTER, SLOWER, INCONCLUSIVE, FASTER_SMALL, SLOWER_SMALL)


def t975(df: int) -> float:
    if df < 1:
        return math.inf
    eligible = [key for key in T975 if key <= df]
    return T975[max(eligible)] if df <= 120 else 1.96


def sign_test_p(wins: int, losses: int) -> float:
    """Exact two-sided sign test; ties are left out. Four wins out of four
    is p = 0.125, never evidence on its own."""
    n = wins + losses
    if n == 0:
        return 1.0
    tail = sum(math.comb(n, k) for k in range(0, min(wins, losses) + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def spearman(xs: Sequence[float], ys: Sequence[float]) -> float:
    """Rank correlation (average ranks for ties); 0 when undefined."""
    def ranks(values):
        order = sorted(range(len(values)), key=lambda i: values[i])
        out = [0.0] * len(values)
        i = 0
        while i < len(order):
            j = i
            while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
                j += 1
            for k in range(i, j + 1):
                out[order[k]] = (i + j) / 2
            i = j + 1
        return out
    if len(xs) != len(ys) or len(xs) < 3:
        return 0.0
    rx, ry = ranks(xs), ranks(ys)
    mx, my = sum(rx) / len(rx), sum(ry) / len(ry)
    cov = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    vx, vy = sum((a - mx) ** 2 for a in rx), sum((b - my) ** 2 for b in ry)
    return cov / math.sqrt(vx * vy) if vx and vy else 0.0


def spearman_p(xs: Sequence[float], ys: Sequence[float], resamples: int = 20000, seed: int = 1) -> float:
    """Two-sided permutation p value of the rank correlation: how often
    shuffled values correlate at least as strongly by chance."""
    observed = abs(spearman(xs, ys))
    if observed == 0.0:
        return 1.0
    rng = random.Random(seed)
    shuffled = list(ys)
    hits = 0
    for _ in range(resamples):
        rng.shuffle(shuffled)
        hits += abs(spearman(xs, shuffled)) >= observed - 1e-12
    return (hits + 1) / (resamples + 1)


@dataclass(frozen=True)
class Comparison:
    deltas_pct: List[float]   # per pair, (new - base) / base * 100
    mean_pct: float           # the actual mean of the pairs, not a bootstrap mean
    ci_low_pct: float         # the wider of the two intervals below
    ci_high_pct: float
    bootstrap_ci_pct: Tuple[float, float]
    t_ci_pct: Tuple[float, float]
    wins: int                 # pairs where new < base
    pairs: int
    sign_p: float

    def verdict(self, min_effect_pct: float) -> str:
        """`min_effect_pct` is a practical threshold, not a noise floor. An
        interval that excludes zero but not the threshold is a real but
        small difference, which a plain "inconclusive" would hide."""
        if self.ci_high_pct < -min_effect_pct:
            return FASTER
        if self.ci_low_pct > min_effect_pct:
            return SLOWER
        if self.ci_high_pct < 0:
            return FASTER_SMALL
        if self.ci_low_pct > 0:
            return SLOWER_SMALL
        return INCONCLUSIVE

    def to_json(self) -> dict:
        return {"deltas_pct": self.deltas_pct, "mean_pct": self.mean_pct,
                "ci95_pct": [self.ci_low_pct, self.ci_high_pct],
                "ci95_bootstrap_pct": list(self.bootstrap_ci_pct), "ci95_t_pct": list(self.t_ci_pct),
                "wins": self.wins, "pairs": self.pairs, "sign_test_p": self.sign_p}


def paired(base: Sequence[float], new: Sequence[float], resamples: int = 10000,
           seed: int = 1) -> Comparison:
    if len(base) != len(new) or not base:
        raise ValueError("need the same, nonzero number of base and new values")
    deltas = [(n - b) / b * 100 for b, n in zip(base, new)]
    count = len(deltas)
    mean = sum(deltas) / count
    rng = random.Random(seed)
    means = sorted(sum(rng.choices(deltas, k=count)) / count for _ in range(resamples))
    boot = (means[int(0.025 * resamples)], means[int(0.975 * resamples) - 1])
    if count > 1:
        sd = math.sqrt(sum((d - mean) ** 2 for d in deltas) / (count - 1))
        half = t975(count - 1) * sd / math.sqrt(count)
    else:
        half = math.inf
    t_ci = (mean - half, mean + half)
    wins = sum(n < b for b, n in zip(base, new))
    losses = sum(n > b for b, n in zip(base, new))
    return Comparison(
        deltas_pct=deltas, mean_pct=mean,
        ci_low_pct=min(boot[0], t_ci[0]), ci_high_pct=max(boot[1], t_ci[1]),
        bootstrap_ci_pct=boot, t_ci_pct=t_ci,
        wins=wins, pairs=count, sign_p=sign_test_p(wins, losses),
    )
