"""Paired A/B comparison of two emulator configurations."""
from __future__ import annotations

import json
from dataclasses import dataclass, replace
from pathlib import Path
from typing import List, Optional

from . import stats, trace
from .marks import workload_diff
from .runner import RunResult, RunSpec, run

# A wall-time pair is clean when neither run lost real time off-CPU:
# wall <= 1.05 x CPU and at most 1 s of off-CPU time.
CLEAN_WALL_OVER_CPU = 1.05
CLEAN_OFFCPU_MS = 1000.0


@dataclass
class Side:
    name: str          # "base" or "new"
    binary: Path       # snapshot
    args: List[str]
    sha256: str


def _metrics(result: RunResult, begin: str, end: str) -> dict:
    p = trace.phase(trace.load(str(result.trace_path)), begin, end)
    return {"wall_ms": p.wall_ms, "cpu_ms": p.cpu_ms, "offcpu_ms": p.offcpu_ms,
            "compile_wall_ms": p.compile_wall_ms, "compile_cpu_ms": p.compile_cpu_ms,
            "noncompile_cpu_ms": p.noncompile_cpu_ms, "noncompile_wall_ms": p.noncompile_wall_ms}


def _clean(m: dict) -> bool:
    return m["wall_ms"] <= CLEAN_WALL_OVER_CPU * m["cpu_ms"] and m["offcpu_ms"] <= CLEAN_OFFCPU_MS


def compare(base: Side, new: Side, spec: RunSpec, pairs: int, out: Path, phase: tuple,
            min_effect_pct: float, allow_workload_change: bool, seed: int = 1) -> dict:
    if pairs < 4 or pairs % 2:
        raise ValueError("use an even number of pairs, at least 4, so AB and BA orders balance")
    report = {"schema": "op1prof.ab.v1", "phase": list(phase), "min_effect_pct": min_effect_pct,
              "sides": {s.name: {"binary_sha256": s.sha256, "args": s.args} for s in (base, new)},
              "runs": [], "pairs": [], "status": "running"}
    path = out / "results.json"

    def save():
        path.write_text(json.dumps(report, indent=1))

    save()
    results = []
    for index in range(pairs):
        order = (base, new) if (index + seed) % 2 == 0 else (new, base)
        pair = {}
        for side in order:
            side_spec = replace(spec, binary=side.binary, args=spec.args + side.args)
            result = run(side_spec, out / f"pair{index:02d}-{side.name}")
            entry = {"pair": index, "side": side.name, **result.to_json()}
            report["runs"].append(entry)
            save()
            if not result.reached:
                # Never drop a failed run and carry on: the batch has no verdict.
                report["status"] = f"failed: pair {index} {side.name}: {result.error}"
                save()
                return report
            entry["metrics"] = _metrics(result, *phase)
            pair[side.name] = (result, entry["metrics"])
            save()
        results.append(pair)
        report["pairs"].append({
            "pair": index, "order": [s.name for s in order],
            "workload_diff": workload_diff(pair["base"][0].marks, pair["new"][0].marks),
            "clean": _clean(pair["base"][1]) and _clean(pair["new"][1]),
        })
        save()

    # Determinism inside each side: every run of one configuration must do the
    # same guest work, or the configuration is not measurable this way.
    unstable = []
    for name in ("base", "new"):
        first = results[0][name][0].marks
        for pair in results[1:]:
            unstable += [f"{name}: {line}" for line in workload_diff(first, pair[name][0].marks)]
    report["unstable"] = unstable

    workload_changed = [p for p in report["pairs"] if p["workload_diff"]]
    report["workload_changed"] = bool(workload_changed)
    summary = {}
    for metric in ("noncompile_cpu_ms", "compile_cpu_ms", "cpu_ms"):
        comparison = stats.paired([p["base"][1][metric] for p in results],
                                  [p["new"][1][metric] for p in results], seed=seed)
        summary[metric] = {**comparison.to_json(), "verdict": comparison.verdict(min_effect_pct)}
    clean = [p for p, info in zip(results, report["pairs"]) if info["clean"]]
    if len(clean) >= 4:
        comparison = stats.paired([p["base"][1]["noncompile_wall_ms"] for p in clean],
                                  [p["new"][1]["noncompile_wall_ms"] for p in clean], seed=seed)
        summary["noncompile_wall_ms"] = {**comparison.to_json(), "verdict": comparison.verdict(min_effect_pct),
                                         "clean_pairs": len(clean)}
    else:
        summary["noncompile_wall_ms"] = {"skipped": f"only {len(clean)} clean pairs"}
    report["summary"] = summary

    if unstable:
        report["status"] = "unusable: a configuration did not repeat its own workload"
    elif workload_changed and not allow_workload_change:
        report["status"] = "workload changed: the two sides did different guest work"
    else:
        report["status"] = summary["noncompile_cpu_ms"]["verdict"]
    save()
    return report
