"""One binary run with several settings, a few times each: what a setting
costs and buys in compile time, the worker's time, execution and wall time.

The configurations come from --param FLAG=V1,V2,... (every combination of
the values of every such flag) and --config NAME=ARGS (one each). Runs are
serial (two emulators at once share the USB/IP port and the CPU), the
configurations in order in the first round and in reverse in the next, so
that a machine that drifts over the sweep moves no configuration more than
another. Each configuration's runs must do the same guest work; across
configurations a different workload is reported, not an error (a setting
may change how often the guest polls, say).

This is a survey: for a decision between two settings, ab's paired runs
and their interval are the evidence.
"""
from __future__ import annotations

import itertools
import json
import re
import statistics
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from . import trace
from .marks import workload_diff
from .runner import RunSpec, run

SCHEMA = "op1prof.sweep.v1"
TIER_RE = re.compile(r"^\[tier\] (?P<name>\S+) .*\bwait_ms=(?P<wait>[0-9.]+)")
# Columns of the summary: (key, header, scale, format).
COLUMNS = (("compile_cpu_ms", "compile s", 1e-3, "{:9.2f}"),
           ("bg_compile_cpu_ms", "worker s", 1e-3, "{:9.2f}"),
           ("jit_cpu_ms", "all JIT s", 1e-3, "{:9.2f}"),
           ("noncompile_cpu_ms", "exec s", 1e-3, "{:8.3f}"),
           ("tier_wait_ms", "wait s", 1e-3, "{:7.2f}"),
           ("wall_ms", "wall s", 1e-3, "{:8.2f}"),
           ("compile_insns", "comp Gi", 1e-9, "{:8.1f}"),
           ("noncompile_insns", "exec Gi", 1e-9, "{:8.2f}"))
# Columns that hold the worker's CPU: estimates where a run's is (bg_exact).
ESTIMATED = ("bg_compile_cpu_ms", "jit_cpu_ms")


def parse_config(text: str) -> Tuple[str, List[str]]:
    name, sep, args = text.partition("=")
    if not sep or not name:
        raise ValueError(f"a configuration is NAME=ARGS, got {text!r}")
    return name, args.split()


def parse_param(text: str) -> Tuple[str, List[str]]:
    """FLAG=V1,V2,...; the flag with or without its dashes (argparse takes
    a value that starts with them for an option: --param=--flag=... or
    --param flag=...)."""
    flag, sep, values = text.partition("=")
    flag = flag if flag.startswith("-") else "--" + flag
    if not sep or flag.strip("-") == "" or not values or "" in values.split(","):
        raise ValueError(f"a parameter is FLAG=V1,V2,..., got {text!r}")
    return flag, values.split(",")


def configurations(params: Sequence[Tuple[str, List[str]]],
                   configs: Sequence[Tuple[str, List[str]]]) -> List[Tuple[str, List[str]]]:
    """Every combination of the parameters' values, then the named ones."""
    out = []
    if params:
        for values in itertools.product(*(values for _, values in params)):
            name = ",".join(f"{flag.lstrip('-')}={value}" for (flag, _), value in zip(params, values))
            args = [part for (flag, _), value in zip(params, values) for part in (flag, value)]
            out.append((name, args))
    out += list(configs)
    names = [name for name, _ in out]
    if len(set(names)) != len(names):
        raise ValueError(f"configuration names repeat: {names}")
    if not out:
        raise ValueError("nothing to sweep: give --param or --config")
    return out


def tier_wait_ms(log_lines: Sequence[str], begin: str, end: str) -> Optional[float]:
    """The CPU thread's wait for the JIT worker between two marks: the [tier]
    lines' wait_ms is cumulative. None without either line."""
    waits = {}
    for line in log_lines:
        match = TIER_RE.match(line.strip())
        if match:
            waits[match.group("name")] = float(match.group("wait"))
    if begin not in waits or end not in waits:
        return None
    return waits[end] - waits[begin]


@dataclass
class SweepRun:
    config: str
    round: int
    directory: Path
    error: Optional[str]
    metrics: Dict[str, Optional[float]]

    def to_json(self) -> dict:
        return {"config": self.config, "round": self.round, "directory": str(self.directory), "error": self.error,
                "metrics": self.metrics}


def _metrics(result, phase: Tuple[str, str]) -> Dict[str, Optional[float]]:
    p = trace.phase(trace.load(str(result.trace_path)), *phase)
    log = (result.directory / "log.txt").read_text(errors="replace").splitlines()
    return {"compile_cpu_ms": p.compile_cpu_ms, "bg_compile_cpu_ms": p.bg_compile_cpu_ms, "jit_cpu_ms": p.jit_cpu_ms,
            "noncompile_cpu_ms": p.noncompile_cpu_ms, "wall_ms": p.wall_ms,
            "tier_wait_ms": tier_wait_ms(log, *phase),
            "compile_insns": p.compile_insns, "noncompile_insns": p.noncompile_insns,
            # False: the worker's CPU (and so all JIT) is an estimate (trace.bg_cpu_ms).
            "bg_exact": p.bg_exact}


def sweep(spec: RunSpec, out: Path, configs: List[Tuple[str, List[str]]], rounds: int,
          phase: Tuple[str, str]) -> dict:
    if rounds < 1:
        raise ValueError("a sweep needs at least one round")
    report = {"schema": SCHEMA, "phase": list(phase), "configurations": [{"name": n, "args": a} for n, a in configs],
              "runs": [], "workload": {}, "status": "running"}
    path = out / "results.json"
    marks_of: Dict[str, list] = {}
    for round_ in range(rounds):
        order = configs if round_ % 2 == 0 else list(reversed(configs))
        for name, args in order:
            directory = out / f"r{round_}-{len(report['runs']):03d}"
            result = run(replace(spec, args=spec.args + args), directory)
            if result.reached:
                entry = SweepRun(name, round_, directory, None, _metrics(result, phase))
                first = marks_of.setdefault(name, result.marks)
                problems = workload_diff(first, result.marks)
                if problems:
                    entry.error = "another workload than its first round: " + "; ".join(problems[:3])
            else:
                entry = SweepRun(name, round_, directory, result.error, {})
            report["runs"].append(entry.to_json())
            path.write_text(json.dumps(report, indent=1))
    # Across configurations: informational.
    names = [name for name, _ in configs]
    reference = next((n for n in names if n in marks_of), None)
    for name in names:
        if name in marks_of and name != reference:
            report["workload"][name] = workload_diff(marks_of[reference], marks_of[name])
    report["reference"] = reference
    report["summary"] = summarize(report)
    failed = [r for r in report["runs"] if r["error"]]
    report["status"] = "done" if not failed else f"{len(failed)} runs failed or did other work"
    path.write_text(json.dumps(report, indent=1))
    return report


def summarize(report: dict) -> List[dict]:
    rows = []
    for config in report["configurations"]:
        runs = [r for r in report["runs"] if r["config"] == config["name"] and not r["error"]]
        row = {"config": config["name"], "n": len(runs),
               "worker_estimated": sum(1 for r in runs if r["metrics"].get("bg_exact") is False)}
        for key, *_ in COLUMNS:
            values = [r["metrics"][key] for r in runs if r["metrics"].get(key) is not None]
            row[key] = {"mean": statistics.fmean(values), "min": min(values), "max": max(values)} if values else None
        rows.append(row)
    return rows


def format_sweep(report: dict) -> str:
    width = max([6] + [len(row["config"]) for row in report["summary"]])
    header = f"{'config':{width}s} {'n':>2} " + " ".join(f"{h:>{len(fmt.format(0))}}" for _, h, _, fmt in COLUMNS)
    lines = [f"sweep {report['phase'][0]} -> {report['phase'][1]}: means over the rounds "
             "(a survey; decide between two settings with ab)", header]
    for row in report["summary"]:
        cells = []
        for key, _, scale, fmt in COLUMNS:
            value = row[key]
            cell = fmt.format(value["mean"] * scale) if value else f"{'-':>{len(fmt.format(0))}}"
            if value and key in ESTIMATED and row.get("worker_estimated"):
                cell = "~" + cell[1:] if cell.startswith(" ") else "~" + cell
            cells.append(cell)
        lines.append(f"{row['config']:{width}s} {row['n']:>2} " + " ".join(cells))
    spread = [(row["config"], key, row[key]) for row in report["summary"] for key, *_ in COLUMNS[:4]
              if row[key] and row[key]["mean"] and (row[key]["max"] - row[key]["min"]) > 0.05 * row[key]["mean"]]
    for config, key, value in spread:
        lines.append(f"{config}: {key} spread over 5% between rounds "
                     f"({value['min'] / 1e3:.3f}..{value['max'] / 1e3:.3f} s): a noisy machine, or too few rounds")
    for row in report["summary"]:
        if row.get("worker_estimated"):
            lines.append(f"{row['config']}: ~ worker s and all JIT s are estimates in {row['worker_estimated']} of "
                         f"{row['n']} runs (no worker clock at the marks, and promotions crossed them)")
    for name, problems in report["workload"].items():
        if problems:
            lines.append(f"{name}: other guest work than {report['reference']}: " + "; ".join(problems[:3]))
    for r in report["runs"]:
        if r["error"]:
            lines.append(f"{r['config']} round {r['round']}: {r['error']}")
    lines.append(report["status"])
    return "\n".join(lines)
