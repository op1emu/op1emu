"""Phase marks and the counters they carry.

The emulator prints one line per mark, cumulative since the `start` mark:

    [mark] NAME pc=0x... wall_ms=... cpu_ms=... runs=... packets=... ...
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Dict, Iterable, List

MARK_RE = re.compile(r"^\[mark\] (?P<name>[A-Za-z0-9_.-]+) (?P<fields>.*)$")

# Guest-visible work. With --deterministic and the same inputs these are
# identical on every run; a change that moves them changed what the guest did.
# `irq` (interrupts and exceptions taken) is missing from binaries older than
# it, and is compared only when both sides report it.
WORKLOAD = ("packets", "cycles", "mmio_r", "mmio_w", "events", "irq")
# Host-side shape of the same work: legitimately changed by an optimization
# that, say, forms longer blocks. Reported, never gated.
SHAPE = ("runs", "translated")
# Host cost (--profile-counters, all threads' CPU): what is being measured,
# never part of a workload comparison.
HOST_COST = ("host_insns", "host_cycles", "process_cpu_ms")


@dataclass(frozen=True)
class Mark:
    name: str
    pc: int
    wall_ms: float
    cpu_ms: float
    counters: Dict[str, int] = field(default_factory=dict)
    deterministic: bool = False

    def workload(self) -> Dict[str, int]:
        return {key: self.counters[key] for key in WORKLOAD if key in self.counters}


def parse_mark(line: str) -> Mark | None:
    match = MARK_RE.match(line.strip())
    if not match:
        return None
    values = dict(item.split("=", 1) for item in match.group("fields").split())
    try:
        return Mark(
            name=match.group("name"),
            pc=int(values.pop("pc"), 16),
            wall_ms=float(values.pop("wall_ms")),
            cpu_ms=float(values.pop("cpu_ms")),
            deterministic=values.pop("deterministic", "0") == "1",
            counters={key: float(value) if "." in value else int(value) for key, value in values.items()},
        )
    except (KeyError, ValueError) as error:
        raise ValueError(f"malformed mark line: {line.strip()!r}") from error


def parse_marks(lines: Iterable[str]) -> List[Mark]:
    marks = [mark for mark in map(parse_mark, lines) if mark]
    names = [mark.name for mark in marks]
    duplicates = {name for name in names if names.count(name) > 1}
    if duplicates:
        raise ValueError(f"marks fired more than once: {sorted(duplicates)}")
    return marks


def optional_absent(*sides: List[Mark]) -> set:
    """Counters some side never reports at all: a binary older than them.
    They are left out of a comparison, but a side that reports one at some
    marks and not at others is a difference, not an older binary."""
    return {key for key in WORKLOAD
            if any(side and not any(key in m.counters for m in side) for side in sides)}


def workload_diff(left: List[Mark], right: List[Mark]) -> List[str]:
    """Differences in fired marks and their workload, as readable lines."""
    problems = []
    left_by, right_by = {m.name: m for m in left}, {m.name: m for m in right}
    skipped = optional_absent(left, right)
    if [m.name for m in left] != [m.name for m in right]:
        problems.append(f"marks differ: {[m.name for m in left]} vs {[m.name for m in right]}")
    for name in left_by.keys() & right_by.keys():
        a, b = left_by[name].workload(), right_by[name].workload()
        for key in WORKLOAD:
            if key in skipped:
                continue
            if key not in a or key not in b:
                problems.append(f"{name}.{key}: missing on {'the left' if key not in a else 'the right'}")
                continue
            if a[key] != b[key]:
                delta = (b[key] - a[key]) / a[key] * 100 if a[key] else float("inf")
                problems.append(f"{name}.{key}: {a[key]} vs {b[key]} ({delta:+.3f}%)")
    return problems
