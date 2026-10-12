"""Repeat one configuration and check it does the same guest work every time."""
from __future__ import annotations

import json
from pathlib import Path
from typing import List, Optional

from .marks import SHAPE, WORKLOAD, Mark
from . import diverge
from .census import Symbols
from .runner import RunSpec, run


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
    for index, other in enumerate(tuples[1:], 1):
        lines = _diff(tuples[0], other)
        report["differences"] += [f"run {index}: {line}" for line in lines]
        if lines and spec.checksum_every:
            # Where the runs first differ, from their guest-state checksums.
            report.setdefault("divergence", {})[str(index)] = diverge.report(
                out / "run00", out / f"run{index:02d}", Symbols(None))
    if expect:
        saved = json.loads(expect.read_text())
        expected = saved["marks"]
        # Older expect files carry no inputs; when they do, a different
        # image explains a different workload before any counter does.
        inputs = report["runs"][0]["inputs"]
        for key, value in saved.get("inputs", {}).items():
            if inputs.get(key) != value:
                report["differences"].append(f"expected: input {key}: {value} vs {inputs.get(key)}")
        report["differences"] += [f"expected: {line}" for line in _diff(expected, tuples[0])]
    if write_expect and not report["differences"]:
        write_expect.write_text(json.dumps({"schema": "op1prof.expect.v1", "inputs": report["runs"][0]["inputs"],
                                             "marks": tuples[0]}, indent=1) + "\n")
    report["status"] = "deterministic" if not report["differences"] else "differences found"
    path.write_text(json.dumps(report, indent=1))
    return report
