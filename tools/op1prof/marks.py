"""Phase marks and the counters they carry.

The emulator prints one line per mark, cumulative since the `start` mark:

    [mark] NAME pc=0x... wall_ms=... cpu_ms=... runs=... packets=... ...
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

MARK_RE = re.compile(r"^\[mark\] (?P<name>[A-Za-z0-9_.-]+) (?P<fields>.*)$")

# Guest-visible work. With --deterministic and the same inputs these are
# identical on every run; a change that moves them changed what the guest did.
# `irq` (interrupts and exceptions taken) and `irq_latched` (host-raised
# interrupts that set a new ILAT bit) are missing from binaries older than
# them, and are compared only when both sides report them.
WORKLOAD = ("packets", "cycles", "mmio_r", "mmio_w", "irq", "irq_latched")
# Host-side shape of the same work: legitimately changed by an optimization
# that, say, forms longer blocks. Reported, never gated. `events` counts the
# CPU queue's callbacks: an asserted interrupt level is raised again after
# every block until the guest takes it, so delivering fewer of those
# repeats changes nothing the guest sees. `invalidations` counts JIT cache
# flushes (at every boot ROM entry).
SHAPE = ("runs", "translated", "events", "invalidations")
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


def workload_diff(left: List[Mark], right: List[Mark], skipped: Optional[set] = None) -> List[str]:
    """Differences in fired marks and their workload, as readable lines.
    `skipped`, the counters an older binary lacks, defaults to those of
    these marks; a caller comparing a few marks of longer runs passes the
    runs' own (optional_absent over all their marks)."""
    problems = []
    left_by, right_by = {m.name: m for m in left}, {m.name: m for m in right}
    if skipped is None:
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


def parse_phase(text: str) -> Tuple[str, str]:
    begin, _, end = text.partition(":")
    if not begin or not end:
        raise ValueError(f"a phase is BEGIN:END, got {text!r}")
    return begin, end


# Fired by --profile-end-cycles, not by the marks file.
END_MARK = "end"


def mark_order(marks_file: Path) -> List[str]:
    """Marks in the order a marks file lists them, the frame mark last. The
    profiler does not impose it (any armed range can fire first), so it is a
    contract on the file that perf.check_fired() verifies after each capture."""
    config = json.loads(Path(marks_file).read_text())
    order = ["start"] + [m["name"] for m in config["marks"]]
    if "frame" in config:
        order.append(config["frame"]["name"])
    # The profiler's own mark at a packet count (--end-cycles), always last.
    order.append(END_MARK)
    return order


def check_phase(marks_file: Path, phase: Tuple[str, str], until: str) -> Optional[str]:
    """Before any run: the phase and --until name marks of the file, the
    phase is not empty, and the run does not stop before the phase ends."""
    order = mark_order(marks_file)
    begin, end = phase
    for name in (begin, end, until):
        if name not in order:
            return f"unknown mark {name!r} (the marks file has {order})"
    # An inverted window records nothing and looks merely empty, not wrong.
    if order.index(begin) >= order.index(end):
        return f"phase {begin}:{end} is empty: {begin} does not come before {end}"
    # Stopping first would leave the phase open and the end mark missing.
    if order.index(until) < order.index(end):
        return f"--until {until} comes before the phase end {end}"
    return None
