"""A change that must not change what the guest does, checked on the boot.

The same binary runs several ways, one after the other: as it is
(chained), with every block returning to the host (--no-chain), and with
blocks that only report where they would have chained
(--profile-chain-dry), plus whatever variants the caller adds (say
--no-loop-blocks). Optionally an older binary runs as it is too. Every run
prints the guest's state at every mark (--profile-mark-digest: registers,
DAG, system registers, CEC, RAM): all must agree with the reference, and
so must the counted work (packets, cycles, MMIO, interrupts). In every run
the chain counters' violations must be 0: a block that chained where it
should have returned to the host for work that was due.
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from . import trace
from .marks import WORKLOAD, Mark, workload_diff
from .runner import RunSpec, run

SCHEMA = "op1prof.gate.v1"
DIGEST_ARG = "--profile-mark-digest"
DIGEST_RE = re.compile(r"^\[digest\] (?P<name>\S+) (?P<state>.*)$")
CHAIN_RE = re.compile(r"^\[chain\] (?P<name>\S+) (?P<fields>.*)$")
# The variants every gate runs, after the plain one: the same guest work
# with chaining off, and with chaining only counted.
DEFAULT_VARIANTS = (("no-chain", ["--no-chain"]), ("chain-dry", ["--profile-chain-dry"]))


def digests(lines) -> Dict[str, str]:
    """The [digest] line of every mark: the guest's state there."""
    out = {}
    for line in lines:
        match = DIGEST_RE.match(line.strip())
        if match:
            out[match.group("name")] = match.group("state").strip()
    return out


def chains(lines) -> Dict[str, Dict[str, int]]:
    """The [chain] counters of every mark (returns by reason, cumulative)."""
    out = {}
    for line in lines:
        match = CHAIN_RE.match(line.strip())
        if match:
            out[match.group("name")] = {key: int(value) for key, value in
                                        (item.split("=", 1) for item in match.group("fields").split())}
    return out


def parse_variant(text: str) -> Tuple[str, List[str]]:
    """NAME=ARGS (ARGS split at spaces), as --variant takes it."""
    name, sep, args = text.partition("=")
    if not sep or not name or not args.split():
        raise ValueError(f"a variant is NAME=ARGS, got {text!r}")
    return name, args.split()


@dataclass
class GateRun:
    name: str
    binary: str
    directory: Path
    marks: List[Mark]
    digests: Dict[str, str]
    chains: Dict[str, Dict[str, int]]
    error: Optional[str]
    execution_cpu_s: Optional[float] = None

    def to_json(self) -> dict:
        last = self.marks[-1].name if self.marks else None
        return {"name": self.name, "binary": self.binary, "directory": str(self.directory), "error": self.error,
                "marks": [m.name for m in self.marks], "digests": self.digests,
                "chain": self.chains.get(last, {}) if last else {}, "execution_cpu_s": self.execution_cpu_s}


def _run(name: str, spec: RunSpec, directory: Path, phase: Tuple[str, str]) -> GateRun:
    result = run(spec, directory)
    log = (directory / "log.txt").read_text(errors="replace").splitlines() if (directory / "log.txt").is_file() else []
    out = GateRun(name, str(spec.binary), directory, result.marks, digests(log), chains(log),
                  None if result.reached else result.error)
    if result.reached and spec.trace:
        out.execution_cpu_s = trace.phase(trace.load(str(result.trace_path)), *phase).noncompile_cpu_ms / 1e3
    return out


def compare(reference: GateRun, other: GateRun) -> List[str]:
    """What differs between two runs' guest work and state."""
    if other.error:
        return [f"failed: {other.error}"]
    lines = workload_diff(reference.marks, other.marks)
    for name in [m.name for m in reference.marks]:
        a, b = reference.digests.get(name), other.digests.get(name)
        if a is None or b is None:
            lines.append(f"{name}: no digest on {'the reference' if a is None else 'this run'} "
                         f"(a binary without {DIGEST_ARG})")
        elif a != b:
            fields = [f.split("=")[0] for f, g in zip(a.split(), b.split()) if f != g]
            lines.append(f"{name}: guest state differs ({', '.join(fields)})")
    return lines


def violations(run_: GateRun) -> List[str]:
    """Chain violations at the last mark (the counters are cumulative)."""
    if not run_.marks:
        return []
    last = run_.chains.get(run_.marks[-1].name)
    if last is None:
        return [f"no [chain] line at {run_.marks[-1].name}"]
    found = {key: value for key, value in last.items() if key.startswith("violation") and value}
    return [f"chain violations at {run_.marks[-1].name}: " + ", ".join(f"{k}={v}" for k, v in sorted(found.items()))
            ] if found.get("violations") else []


def gate(spec: RunSpec, out: Path, base: Optional[RunSpec], variants: List[Tuple[str, List[str]]],
         phase: Tuple[str, str]) -> dict:
    """The runs, one after the other (two emulators at once would share the
    USB/IP port and the CPU), and their comparison with the reference: the
    base binary's plain run if there is one, else this binary's."""
    report = {"schema": SCHEMA, "runs": [], "differences": [], "status": "running"}
    path = out / "results.json"
    plan: List[Tuple[str, RunSpec]] = []
    if base is not None:
        plan.append(("base", replace(base, args=base.args + [DIGEST_ARG])))
    plan.append(("plain", replace(spec, args=spec.args + [DIGEST_ARG])))
    for name, args in list(DEFAULT_VARIANTS) + list(variants):
        plan.append((name, replace(spec, args=spec.args + args + [DIGEST_ARG])))
    runs: List[GateRun] = []
    for index, (name, config) in enumerate(plan):
        done = _run(name, config, out / f"{index:02d}-{name}", phase)
        runs.append(done)
        report["runs"].append(done.to_json())
        path.write_text(json.dumps(report, indent=1))
    reference = runs[0]
    if reference.error:
        report["differences"].append(f"{reference.name}: failed: {reference.error}")
    for done in runs:
        found = ([] if done is reference else compare(reference, done)) + violations(done)
        report["differences"] += [f"{done.name}: {line}" for line in found]
    report["reference"] = reference.name
    report["status"] = "same" if not report["differences"] else "differences found"
    path.write_text(json.dumps(report, indent=1))
    return report


def format_gate(report: dict) -> str:
    lines = [f"{'run':14s} {'marks':>5} {'returns':>12} {'chained':>12} {'would':>12} {'violations':>10} "
             f"{'exec s':>8}  result"]
    differing = {line.split(":", 1)[0] for line in report["differences"]}
    for r in report["runs"]:
        c = r["chain"]
        exec_s = f"{r['execution_cpu_s']:8.3f}" if r["execution_cpu_s"] is not None else f"{'-':>8}"
        verdict = ("reference" if r["name"] == report.get("reference") and r["name"] not in differing
                   else "differs" if r["name"] in differing else "same")
        lines.append(f"{r['name']:14s} {len(r['marks']):>5} {c.get('returns', 0):>12,} {c.get('chained', 0):>12,} "
                     f"{c.get('would', 0):>12,} {c.get('violations', 0):>10} {exec_s}  {verdict}")
    lines += [""] + report["differences"]
    lines.append(f"{report['status']}: guest work ({', '.join(WORKLOAD)}) and state at every mark")
    return "\n".join(lines)
