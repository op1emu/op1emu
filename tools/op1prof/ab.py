"""Paired A/B comparison of two emulator configurations."""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass, fields as dataclass_fields, replace
from pathlib import Path
from typing import List, Optional, Tuple

from . import machine, stats, trace
from .marks import Mark, workload_diff
from .runner import RunResult, RunSpec, input_hashes, run, sha256_file

# A wall-time pair is clean when neither run lost real time off-CPU:
# wall <= 1.05 x CPU and at most 1 s of off-CPU time.
CLEAN_WALL_OVER_CPU = 1.05
CLEAN_OFFCPU_MS = 1000.0
# A trend with the run order this unlikely by chance (permutation test on the
# rank correlation), over at least this many runs per side, means the machine
# changed during the batch: compile time once climbed from 220 s to 270 s and
# took execution time with it. Four tests per batch, so a strict threshold.
DRIFT_P = 0.01
DRIFT_MIN_RUNS = 6
SCHEMA = "op1prof.ab.v2"


@dataclass
class Side:
    name: str          # "base" or "new"
    binary: Path       # snapshot
    args: List[str]
    sha256: str


def _metrics(result: RunResult, begin: str, end: str) -> dict:
    return trace.phase(trace.load(str(result.trace_path)), begin, end).to_json()


def _clean(m: dict) -> bool:
    return m["wall_ms"] <= CLEAN_WALL_OVER_CPU * m["cpu_ms"] and m["offcpu_ms"] <= CLEAN_OFFCPU_MS


def _contended(entry: dict) -> List[str]:
    """Reasons a run's CPU time includes more than its own work."""
    contention = entry.get("contention") or {}
    reasons = []
    busy = contention.get("sibling_busy")
    if busy is not None and busy > machine.SIBLING_BUSY:
        reasons.append(f"SMT sibling {busy:.0%} busy")
    if contention.get("throttle_events"):
        reasons.append(f"{contention['throttle_events']} thermal throttling events")
    return reasons


def _marks(entry: dict) -> List[Mark]:
    keys = ("name", "pc", "wall_ms", "cpu_ms")
    return [Mark(name=m["name"], pc=m["pc"], wall_ms=m["wall_ms"], cpu_ms=m["cpu_ms"],
                 counters={k: v for k, v in m.items() if k not in keys}) for m in entry["marks"]]


def _config(base: Side, new: Side, spec: RunSpec, phase: tuple, min_effect_pct: float,
            allow_workload_change: bool, seed: int) -> dict:
    """Everything `extend` needs to add pairs to the same batch."""
    fields = {k: (str(v) if isinstance(v, Path) else v) for k, v in asdict(spec).items() if k != "binary"}
    return {"sides": {s.name: {"binary": str(s.binary), "args": s.args, "sha256": s.sha256} for s in (base, new)},
            "spec": fields, "inputs": input_hashes(spec), "phase": list(phase), "min_effect_pct": min_effect_pct,
            "allow_workload_change": allow_workload_change, "seed": seed}


def from_config(config: dict) -> Tuple[Side, Side, RunSpec]:
    sides = []
    for name in ("base", "new"):
        side = config["sides"][name]
        binary = Path(side["binary"])
        if not binary.is_file() or sha256_file(binary) != side["sha256"]:
            raise ValueError(f"the {name} snapshot {binary} is missing or changed; the batch cannot be extended")
        sides.append(Side(name, binary, list(side["args"]), side["sha256"]))
    known = {f.name for f in dataclass_fields(RunSpec)} - {"binary"}
    fields = {k: v for k, v in config["spec"].items() if k in known}
    for key in ("nand", "otp", "marks"):
        fields[key] = Path(fields[key])
    spec = RunSpec(binary=sides[0].binary, **fields)
    # New pairs must boot the same images as the old ones, or they measure
    # a different workload under the same interval.
    now = input_hashes(spec)
    for key, value in config.get("inputs", {}).items():
        if now.get(key) != value:
            raise ValueError(f"input {key} changed since the batch started ({value} -> {now.get(key)})")
    return sides[0], sides[1], spec


def compare(base: Side, new: Side, spec: RunSpec, pairs: int, out: Path, phase: tuple,
            min_effect_pct: float, allow_workload_change: bool, seed: int = 1,
            previous: Optional[dict] = None) -> dict:
    """Run `pairs` alternating pairs into `out`. With `previous` (an earlier
    report of the same batch), add them to its pairs instead of starting."""
    if pairs < 2 or pairs % 2 or (previous is None and pairs < 4):
        raise ValueError("use an even number of pairs, at least 4 (2 to extend), so AB and BA orders balance")
    if previous is None:
        report = {"schema": SCHEMA, "phase": list(phase), "min_effect_pct": min_effect_pct,
                  "sides": {s.name: {"binary_sha256": s.sha256, "args": s.args} for s in (base, new)},
                  "config": _config(base, new, spec, phase, min_effect_pct, allow_workload_change, seed),
                  "runs": [], "pairs": [], "extensions": 0, "status": "running"}
    else:
        if previous.get("status") not in stats.VERDICTS + ("workload changed: the two sides did different guest work",):
            raise ValueError(f"the batch ended {previous.get('status')!r}: only a finished batch can be extended; "
                             "start a new one")
        report = {**previous, "extensions": previous.get("extensions", 0) + 1, "status": "running"}
        for key in ("summary", "drift", "contended_pairs", "unstable"):
            report.pop(key, None)
    path = out / "results.json"

    def save():
        path.write_text(json.dumps(report, indent=1))

    save()
    first = len(report["pairs"])
    for index in range(first, first + pairs):
        order = (base, new) if (index + seed) % 2 == 0 else (new, base)
        for side in order:
            side_spec = replace(spec, binary=side.binary, args=spec.args + side.args)
            result = run(side_spec, out / f"pair{index:02d}-{side.name}")
            entry = {"pair": index, "side": side.name, **result.to_json()}
            report["runs"].append(entry)
            if not result.reached:
                # Never drop a failed run and carry on: the batch has no verdict.
                report["status"] = f"failed: pair {index} {side.name}: {result.error}"
                save()
                return report
            entry["metrics"] = _metrics(result, *phase)
            save()
        runs = {e["side"]: e for e in report["runs"] if e["pair"] == index}
        report["pairs"].append({
            "pair": index, "order": [s.name for s in order],
            "workload_diff": workload_diff(_marks(runs["base"]), _marks(runs["new"])),
            "clean": _clean(runs["base"]["metrics"]) and _clean(runs["new"]["metrics"]),
            "contended": [f"{name}: {reason}" for name in ("base", "new") for reason in _contended(runs[name])],
        })
        save()
    summarize(report, min_effect_pct, allow_workload_change, seed)
    save()
    return report


def _drift(report: dict) -> List[str]:
    """Trends with the run order: the machine, not the change, moving."""
    found = []
    for name in ("base", "new"):
        runs = [e for e in report["runs"] if e["side"] == name and "metrics" in e]
        if len(runs) < DRIFT_MIN_RUNS:
            continue
        order = list(range(len(runs)))
        for metric in ("compile_cpu_ms", "noncompile_cpu_ms"):
            series = [e["metrics"][metric] for e in runs]
            rho, p = stats.spearman(order, series), stats.spearman_p(order, series)
            if p < DRIFT_P:
                values = ", ".join(f"{v / 1e3:.1f}" for v in series)
                memory = ", ".join(f"{e['machine_before']['mem_available'] / 2**30:.1f}" for e in runs)
                found.append(f"{name} {metric} trends with run order (rho {rho:+.2f}, p {p:.4f}): {values} s; "
                             f"MemAvailable before each run {memory} GB")
    return found


def summarize(report: dict, min_effect_pct: float, allow_workload_change: bool, seed: int = 1) -> None:
    by_pair = {}
    for entry in report["runs"]:
        by_pair.setdefault(entry["pair"], {})[entry["side"]] = entry
    pairs = [by_pair[p["pair"]] for p in report["pairs"]]

    # Determinism inside each side: every run of one configuration must do the
    # same guest work, or the configuration is not measurable this way.
    unstable = []
    for name in ("base", "new"):
        first = _marks(pairs[0][name])
        for pair in pairs[1:]:
            unstable += [f"{name}: {line}" for line in workload_diff(first, _marks(pair[name]))]
    report["unstable"] = unstable

    workload_changed = [p for p in report["pairs"] if p["workload_diff"]]
    report["workload_changed"] = bool(workload_changed)
    summary = {}
    metrics = ["noncompile_cpu_ms", "compile_cpu_ms", "cpu_ms"]
    # Host instructions (--counters) barely vary between runs, so they can
    # show a change too small for CPU time; they are work, not time, and
    # never decide the status.
    if all(p[side]["metrics"].get("noncompile_insns") is not None for p in pairs for side in ("base", "new")):
        metrics.append("noncompile_insns")
    for metric in metrics:
        comparison = stats.paired([p["base"]["metrics"][metric] for p in pairs],
                                  [p["new"]["metrics"][metric] for p in pairs], seed=seed)
        summary[metric] = {**comparison.to_json(), "verdict": comparison.verdict(min_effect_pct)}
    clean = [p for p, info in zip(pairs, report["pairs"]) if info["clean"]]
    if len(clean) >= 4:
        comparison = stats.paired([p["base"]["metrics"]["noncompile_wall_ms"] for p in clean],
                                  [p["new"]["metrics"]["noncompile_wall_ms"] for p in clean], seed=seed)
        summary["noncompile_wall_ms"] = {**comparison.to_json(), "verdict": comparison.verdict(min_effect_pct),
                                         "clean_pairs": len(clean)}
    else:
        summary["noncompile_wall_ms"] = {"skipped": f"only {len(clean)} clean pairs"}
    # Contention is measured during each run, independent of its result: on
    # one A/A a sibling 19-37% busy cut instructions per cycle from about 2.43
    # to 1.98-2.20 and added 17-29 s to an 83 s execution.
    calm = [p for p, info in zip(pairs, report["pairs"]) if not info.get("contended")]
    decided_on = "noncompile_cpu_ms"
    if len(calm) >= 4 and len(calm) < len(pairs):
        comparison = stats.paired([p["base"]["metrics"]["noncompile_cpu_ms"] for p in calm],
                                  [p["new"]["metrics"]["noncompile_cpu_ms"] for p in calm], seed=seed)
        summary["noncompile_cpu_ms_uncontended"] = {**comparison.to_json(), "verdict": comparison.verdict(min_effect_pct),
                                                    "uncontended_pairs": len(calm)}
        decided_on = "noncompile_cpu_ms_uncontended"
    report["summary"] = summary
    report["decided_on"] = decided_on
    report["drift"] = _drift(report)
    report["contended_pairs"] = sum(1 for p in report["pairs"] if p.get("contended"))

    if unstable:
        report["status"] = "unusable: a configuration did not repeat its own workload"
    elif workload_changed and not allow_workload_change:
        report["status"] = "workload changed: the two sides did different guest work"
    else:
        report["status"] = summary[decided_on]["verdict"]
