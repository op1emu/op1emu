"""Paired comparison statistics.

The error bar comes from repeated paired runs, never from sampling counts:
Poisson noise on perf sample counts understated the real run-to-run spread
by about an order of magnitude (same configuration 2.5% apart, the baseline
11% apart, between two capture sets).
"""
from __future__ import annotations

import random
from dataclasses import dataclass
from typing import List, Sequence


@dataclass(frozen=True)
class Comparison:
    deltas_pct: List[float]   # per pair, (new - base) / base * 100
    mean_pct: float           # the actual mean of the pairs, not a bootstrap mean
    ci_low_pct: float
    ci_high_pct: float
    wins: int                 # pairs where new < base
    pairs: int

    def verdict(self, min_effect_pct: float) -> str:
        """`min_effect_pct` is a practical threshold, not a noise floor."""
        if self.ci_high_pct < -min_effect_pct:
            return "faster"
        if self.ci_low_pct > min_effect_pct:
            return "slower"
        return "inconclusive"

    def to_json(self) -> dict:
        return {"deltas_pct": self.deltas_pct, "mean_pct": self.mean_pct,
                "ci95_pct": [self.ci_low_pct, self.ci_high_pct],
                "wins": self.wins, "pairs": self.pairs}


def paired(base: Sequence[float], new: Sequence[float], resamples: int = 10000,
           seed: int = 1) -> Comparison:
    if len(base) != len(new) or not base:
        raise ValueError("need the same, nonzero number of base and new values")
    deltas = [(n - b) / b * 100 for b, n in zip(base, new)]
    rng = random.Random(seed)
    means = sorted(sum(rng.choices(deltas, k=len(deltas))) / len(deltas) for _ in range(resamples))
    return Comparison(
        deltas_pct=deltas,
        mean_pct=sum(deltas) / len(deltas),
        ci_low_pct=means[int(0.025 * resamples)],
        ci_high_pct=means[int(0.975 * resamples) - 1],
        wins=sum(n < b for b, n in zip(base, new)),
        pairs=len(deltas),
    )
