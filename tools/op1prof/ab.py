"""Paired A/B comparison of two emulator configurations."""
from __future__ import annotations

import json
import statistics
from dataclasses import asdict, dataclass, fields as dataclass_fields, replace
from pathlib import Path
from typing import List, Optional, Tuple

from . import compilation, machine, stats, trace
from .marks import Mark, workload_diff
from .runner import RunResult, RunSpec, input_hashes, instrument_args, run, sha256_file

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
SCHEMA = "op1prof.ab.v3"
# Batches extend() can continue: v2 lacks only the per-phase times, which
# the summary then leaves out.
EXTENDABLE = ("op1prof.ab.v2", SCHEMA)
# Shape counters: how the host formed and ran the same work. Reported side by
# side, never gated.
SHAPE_FIELDS = ("runs", "translated", "events", "invalidations")
WORKLOAD_CHANGED = "workload changed: the two sides did different guest work"


@dataclass
class Side:
    name: str          # "base" or "new"
    binary: Path       # snapshot
    args: List[str]
    sha256: str


def _metrics(result: RunResult, begin: str, end: str) -> Tuple[dict, Optional[dict], List[dict]]:
    """Phase times, what the whole run compiled (None for a binary that does
    not report compile shapes), and the times of every phase between
    consecutive marks."""
    trace_data = trace.load(str(result.trace_path))
    spans = compilation.translations(trace_data)
    shape = compilation.codegen(spans) if compilation.has_shape(spans) else None
    phases = [{"begin": a, "end": b, "noncompile_cpu_ms": p.noncompile_cpu_ms, "compile_cpu_ms": p.compile_cpu_ms}
              for a, b, p in trace.phases(trace_data)]
    return trace.phase(trace_data, begin, end).to_json(), shape, phases


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
    # Here rather than only in the ab command, so that a batch extended from
    # its saved settings is held to the same rule.
    instrumented = instrument_args(spec.args + base.args + new.args)
    if instrumented:
        raise ValueError(f"ab measures time and refuses diagnostic instrumentation {instrumented}: use run --census, "
                         "run --probes, run --checksum or perf for those")
    if previous is None:
        report = {"schema": SCHEMA, "phase": list(phase), "min_effect_pct": min_effect_pct,
                  "sides": {s.name: {"binary_sha256": s.sha256, "args": s.args} for s in (base, new)},
                  "config": _config(base, new, spec, phase, min_effect_pct, allow_workload_change, seed),
                  "runs": [], "pairs": [], "extensions": 0, "status": "running"}
    else:
        if previous.get("schema") not in EXTENDABLE:
            raise ValueError(f"the batch was written as {previous.get('schema')!r}, not one of {EXTENDABLE}: "
                             "start a new one")
        if previous.get("status") not in stats.VERDICTS + (WORKLOAD_CHANGED,):
            raise ValueError(f"the batch ended {previous.get('status')!r}: only a finished batch can be extended; "
                             "start a new one")
        report = {**previous, "schema": SCHEMA, "extensions": previous.get("extensions", 0) + 1, "status": "running"}
        for key in ("summary", "drift", "contended_pairs", "unstable", "phases", "shape"):
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
            changed = _changed_inputs({"config": report.get("config"), "runs": [entry]})
            if changed:
                report["status"] = f"failed: an input changed during the batch: {changed[0]}"
                save()
                return report
            entry["metrics"], entry["codegen"], entry["phases"] = _metrics(result, *phase)
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


def perf_captures(base: Side, new: Side, spec: RunSpec, out: Path, phase: tuple, count: int, event: str,
                  period: Optional[int], call_graph: str) -> Tuple[List[Path], List[Path]]:
    """`count` perf captures of each side over the phase, in alternating
    order like the timing pairs, under out/perf/{base,new}/. They are kept
    out of the verdict: sampling changes timing."""
    from . import perf
    captures: dict = {"base": [], "new": []}
    for index in range(count):
        for side in ((base, new) if index % 2 == 0 else (new, base)):
            directory = out / "perf" / side.name / f"capture{index:02d}"
            directory.parent.mkdir(parents=True, exist_ok=True)
            side_spec = replace(spec, binary=side.binary, args=spec.args + side.args)
            result = perf.record(side_spec, directory, phase, event, period, None, call_graph)
            error = result.error if not result.reached else perf.check_fired(spec.marks, phase,
                                                                           [m.name for m in result.marks])
            if error:
                raise RuntimeError(f"perf capture {index} of {side.name} failed: {error}")
            captures[side.name].append(directory)
    return captures["base"], captures["new"]


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


def balanced(pairs: List[tuple]) -> List[tuple]:
    """(pair, info) entries cut to equal numbers of AB and BA orders, keeping
    the earliest of each: a rule fixed before the results are seen."""
    by_order: dict = {}
    for entry in pairs:
        by_order.setdefault(tuple(entry[1]["order"]), []).append(entry)
    if len(by_order) < 2:
        return []
    keep = min(len(entries) for entries in by_order.values())
    return sorted((e for entries in by_order.values() for e in entries[:keep]), key=lambda e: e[1]["pair"])


def _changed_inputs(report: dict) -> List[str]:
    """Runs that read other NAND, OTP or marks files than the batch began with."""
    expected = (report.get("config") or {}).get("inputs")
    if not expected:
        return []
    return [f"pair {e['pair']} {e['side']}: {key} {expected[key]} -> {e.get('inputs', {}).get(key)}"
            for e in report["runs"] for key in expected if e.get("inputs", {}).get(key) != expected[key]]


# Build properties that make two binaries different kinds of build, not two
# revisions of one: a timing comparison across them measures the build.
BUILD_KIND = ("build_type", "profiling", "compiler")


def _builds(pairs: List[dict]) -> Optional[dict]:
    """What each side's binary was built from (the first run's [build] line;
    a side's runs share one snapshot), and the build properties that differ."""
    builds = {name: pairs[0][name].get("build") for name in ("base", "new")}
    if any(b is None for b in builds.values()):
        return None
    differs = [key for key in BUILD_KIND if builds["base"].get(key) != builds["new"].get(key)]
    return {**builds, "kind_differs": differs}


def _phase_deltas(pairs: List[dict], seed: int) -> List[dict]:
    """Execution and compile CPU of every phase between consecutive marks,
    base against new over all pairs: where a difference lies. Reported, never
    a verdict; phases that not every run has are left out."""
    if not all("phases" in p[side] for p in pairs for side in ("base", "new")):
        return []
    def index(entry):
        return {(x["begin"], x["end"]): x for x in entry["phases"]}
    first = [(x["begin"], x["end"]) for x in pairs[0]["base"]["phases"]]
    sides = [(index(p["base"]), index(p["new"])) for p in pairs]
    out = []
    for key in first:
        if not all(key in a and key in b for a, b in sides):
            continue
        row = {"begin": key[0], "end": key[1]}
        for metric in ("noncompile_cpu_ms", "compile_cpu_ms"):
            base = [a[key][metric] for a, _ in sides]
            new = [b[key][metric] for _, b in sides]
            delta_ms = statistics.median(n - b for b, n in zip(base, new))
            if min(base) > 0:
                comparison = stats.paired(base, new, seed=seed)
                row[metric] = {"median_delta_ms": delta_ms, "mean_pct": comparison.mean_pct,
                               "ci95_pct": [comparison.ci_low_pct, comparison.ci_high_pct]}
            else:
                row[metric] = {"median_delta_ms": delta_ms, "mean_pct": None, "ci95_pct": None}
        out.append(row)
    return out


def _shape(pairs: List[dict], end: str) -> Optional[dict]:
    """How each side formed and ran the work, at the phase end (the first
    pair's runs: a configuration repeats it). Blocks executed changing for
    the same packets means the blocks were formed differently."""
    out = {}
    for name in ("base", "new"):
        marks = {m["name"]: m for m in pairs[0][name]["marks"]}
        if end not in marks:
            return None
        mark = marks[end]
        out[name] = {k: mark[k] for k in SHAPE_FIELDS if k in mark}
        if mark.get("runs"):
            out[name]["packets_per_run"] = mark["packets"] / mark["runs"]
    return out


def _codegen(pairs: List[dict]) -> Optional[dict]:
    """Did the two sides generate different machine code? A knob that never
    reached code generation (a nonzero opt level once changed nothing for
    months) shows up here as identical code, whatever the timings say."""
    digests = {name: [p[name].get("codegen") for p in pairs] for name in ("base", "new")}
    if any(d is None for side in digests.values() for d in side):
        return None
    sets = {name: sorted({d["digest"] for d in side}) for name, side in digests.items()}
    first = {name: side[0] for name, side in digests.items()}
    return {"identical": sets["base"] == sets["new"] and len(sets["base"]) == 1,
            "repeatable": all(len(s) == 1 for s in sets.values()),
            **{name: {"digests": sets[name], "translations": first[name]["translations"],
                      "ir_insns": first[name]["ir_insns"], "object_bytes": first[name]["object_bytes"]}
               for name in ("base", "new")}}


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
    report["changed_inputs"] = _changed_inputs(report)

    # Recomputed from the runs' marks every time, so that the pairs of an
    # extended batch are all judged by this version's gate, whichever
    # version ran them.
    for info, pair in zip(report["pairs"], pairs):
        info["workload_diff"] = workload_diff(_marks(pair["base"]), _marks(pair["new"]))

    workload_changed = [p for p in report["pairs"] if p["workload_diff"]]
    report["workload_changed"] = bool(workload_changed)
    summary = {}
    metrics = ["noncompile_cpu_ms", "compile_cpu_ms", "cpu_ms"]
    # The worker's JIT CPU (bcore's background promotions), beside the CPU
    # thread's: a change that moves compilation off the thread shows in both.
    if any(p[side]["metrics"].get("bg_compile_cpu_ms") for p in pairs for side in ("base", "new")):
        metrics.append("jit_cpu_ms")
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
    # Leaving pairs out must not unbalance the AB and BA orders, or a
    # first-run/second-run effect reads as a difference between the sides
    # (an A/A whose contention fell on one order came out "faster"). Keep
    # the earliest pairs of each order, as many as the scarcer order has.
    calm = balanced([(p, info) for p, info in zip(pairs, report["pairs"]) if not info.get("contended")])
    decided_on = "noncompile_cpu_ms"
    if len(calm) >= 4 and len(calm) < len(pairs):
        comparison = stats.paired([p["base"]["metrics"]["noncompile_cpu_ms"] for p, _ in calm],
                                  [p["new"]["metrics"]["noncompile_cpu_ms"] for p, _ in calm], seed=seed)
        summary["noncompile_cpu_ms_uncontended"] = {**comparison.to_json(), "verdict": comparison.verdict(min_effect_pct),
                                                    "uncontended_pairs": [info["pair"] for _, info in calm]}
        decided_on = "noncompile_cpu_ms_uncontended"
    report["summary"] = summary
    report["decided_on"] = decided_on
    report["codegen"] = _codegen(pairs)
    report["phases"] = _phase_deltas(pairs, seed)
    report["shape"] = _shape(pairs, report["phase"][1])
    report["builds"] = _builds(pairs)
    report["drift"] = _drift(report)
    report["contended_pairs"] = sum(1 for p in report["pairs"] if p.get("contended"))

    if report["changed_inputs"]:
        report["status"] = "unusable: runs read different input images"
    elif unstable:
        report["status"] = "unusable: a configuration did not repeat its own workload"
    elif workload_changed and not allow_workload_change:
        report["status"] = WORKLOAD_CHANGED
    else:
        report["status"] = summary[decided_on]["verdict"]
