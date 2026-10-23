"""Repeat one configuration and check it does the same guest work every time."""
from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path
from typing import List, Optional, Tuple

from .marks import SHAPE, WORKLOAD, Mark
from . import diverge, gate
from .census import Symbols
from .runner import RunSpec, run

EXPECT_SCHEMA = "op1prof.expect.v1"


def _tuple(marks: List[Mark]) -> List[dict]:
    return [{"name": m.name, **{k: m.counters[k] for k in WORKLOAD + SHAPE if k in m.counters}} for m in marks]


def _diff(expected: List[dict], actual: List[dict]) -> List[str]:
    """Every counter of every mark, workload and shape alike: one binary
    and one input must reproduce exactly."""
    lines = []
    if [m["name"] for m in expected] != [m["name"] for m in actual]:
        lines.append(f"marks differ: {[m['name'] for m in expected]} vs {[m['name'] for m in actual]}")
    actual_by = {m["name"]: m for m in actual}
    skipped = {key for key in WORKLOAD + SHAPE
               if not any(key in m for m in expected) or not any(key in m for m in actual)}
    for mark in expected:
        other = actual_by.get(mark["name"])
        if not other:
            continue
        for key in WORKLOAD + SHAPE:
            if key in skipped:
                continue  # a counter the older binary or expect file lacks entirely
            if key not in mark or key not in other:
                lines.append(f"{mark['name']}.{key}: missing on one side")
                continue
            if mark[key] != other[key]:
                lines.append(f"{mark['name']}.{key}: {mark[key]} vs {other[key]}")
    return lines


def verify(spec: RunSpec, runs: int, out: Path, expect: Optional[Path], write_expect: Optional[Path]) -> dict:
    if runs < 1:
        raise ValueError("verify needs at least one run")
    report = {"schema": "op1prof.verify.v1", "runs": [], "differences": [], "status": "running"}
    path = out / "results.json"
    tuples = []
    for index in range(runs):
        result = run(spec, out / f"run{index:02d}")
        report["runs"].append(result.to_json())
        path.write_text(json.dumps(report, indent=1))
        if not result.reached:
            report["status"] = f"failed: run {index}: {result.error}"
            path.write_text(json.dumps(report, indent=1))
            return report
        tuples.append(_tuple(result.marks))
    states = [gate.digests((out / f"run{index:02d}" / "log.txt").read_text(errors="replace").splitlines())
              for index in range(runs)]
    for index, other in enumerate(tuples[1:], 1):
        report["differences"] += [f"run {index}: {line}" for line in _diff(tuples[0], other)]
        # With --arg=--profile-mark-digest: the guest's state at every mark too.
        for name, state in states[0].items():
            if states[index].get(name) != state:
                report["differences"].append(f"run {index}: {name}: guest state differs (--profile-mark-digest)")
        if spec.checksum_every:
            # Guest state, compared on its own: two runs can do the same
            # counted work and still hold different registers.
            first, this = out / "run00" / diverge.FILE, out / f"run{index:02d}" / diverge.FILE
            found = diverge.first_difference(diverge.records(first), diverge.records(this))
            if found:
                report["differences"].append(f"run {index}: guest state differs from checksum {found['index']} "
                                             f"({', '.join(found['fields'])})")
                report.setdefault("divergence", {})[str(index)] = diverge.report(
                    out / "run00", out / f"run{index:02d}", Symbols(None))
    if expect:
        saved = json.loads(expect.read_text())
        if saved.get("schema") != EXPECT_SCHEMA:
            raise ValueError(f"{expect}: not an op1prof expect file ({EXPECT_SCHEMA})")
        expected = saved["marks"]
        # Older expect files carry no inputs; when they do, a different
        # image explains a different workload before any counter does.
        inputs = report["runs"][0]["inputs"]
        for key, value in saved.get("inputs", {}).items():
            if inputs.get(key) != value:
                report["differences"].append(f"expected: input {key}: {value} vs {inputs.get(key)}")
        report["differences"] += [f"expected: {line}" for line in _diff(expected, tuples[0])]
    if write_expect and not report["differences"]:
        write_expect.write_text(json.dumps({"schema": EXPECT_SCHEMA, "inputs": report["runs"][0]["inputs"],
                                             "marks": tuples[0]}, indent=1) + "\n")
    report["status"] = "deterministic" if not report["differences"] else "differences found"
    path.write_text(json.dumps(report, indent=1))
    return report


PARANOID = Path("/proc/sys/kernel/perf_event_paranoid")


def _counters_unavailable() -> Optional[str]:
    try:
        level = int(PARANOID.read_text())
    except (OSError, ValueError):
        return f"cannot read {PARANOID}"
    return None if level <= 2 else f"kernel.perf_event_paranoid is {level} (counters need at most 2)"


def _declares_probes(marks: Path) -> bool:
    try:
        return bool(json.loads(marks.read_text()).get("probes"))
    except (OSError, ValueError, AttributeError):
        return False


def configurations(spec: RunSpec) -> List[Tuple[str, Optional[RunSpec], Optional[str]]]:
    """(name, spec, reason skipped) for every instrumentation setting the
    profiler offers, the plain marks-and-trace run first."""
    plain = replace(spec, census=False, counters=False, probes=False, checksum_every=None, dump_pcs=[])
    probes_skip = None if _declares_probes(spec.marks) else "the marks file declares no probes"
    counters_skip = _counters_unavailable()
    return [
        ("plain", plain, None),
        ("census", replace(plain, census=True), None),
        ("probes", None if probes_skip else replace(plain, probes=True), probes_skip),
        ("checksum", replace(plain, checksum_every=65536), None),
        ("counters", None if counters_skip else replace(plain, counters=True), counters_skip),
        ("no trace", replace(plain, trace=False), None),
    ]


def instrumentation(spec: RunSpec, out: Path) -> dict:
    """One run per instrumentation setting: none may change the guest's
    work. Uninitialized memory once made the same binary run 1,151,352,850
    packets to main-display without a trace and 851 with one."""
    report = {"schema": "op1prof.verify-instrumentation.v1", "runs": [], "configurations": [],
              "differences": [], "status": "running"}
    path = out / "results.json"
    reference = None
    for index, (name, config, skipped) in enumerate(configurations(spec)):
        entry = {"name": name}
        report["configurations"].append(entry)
        if config is None:
            entry["skipped"] = skipped
            path.write_text(json.dumps(report, indent=1))
            continue
        result = run(config, out / f"{index:02d}-{name.replace(' ', '-')}")
        report["runs"].append(result.to_json())
        if not result.reached:
            report["status"] = f"failed: {name}: {result.error}"
            path.write_text(json.dumps(report, indent=1))
            return report
        found = _tuple(result.marks)
        if reference is None:
            reference = found
        entry["differences"] = _diff(reference, found)
        report["differences"] += [f"{name}: {line}" for line in entry["differences"]]
        path.write_text(json.dumps(report, indent=1))
    report["status"] = "deterministic" if not report["differences"] else "differences found"
    path.write_text(json.dumps(report, indent=1))
    return report
