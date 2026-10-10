"""The emulator's op1.trace.v2 trace and the time metrics derived from it."""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Dict, List, Tuple


class TraceError(ValueError):
    pass


@dataclass(frozen=True)
class Phase:
    """Time between two marks, split into compilation and everything else."""

    wall_ms: float          # monotonic time between the marks
    cpu_ms: float           # the CPU thread's CPU time between them
    compile_wall_ms: float  # union of translate spans inside the window
    compile_cpu_ms: float   # CPU time inside those spans

    @property
    def noncompile_cpu_ms(self) -> float:
        """Execution CPU: guest code, devices, dispatch. Unlike wall time it
        does not grow when the thread is descheduled (swap stalls of seconds
        were seen inside otherwise clean runs)."""
        return self.cpu_ms - self.compile_cpu_ms

    @property
    def noncompile_wall_ms(self) -> float:
        return self.wall_ms - self.compile_wall_ms

    @property
    def offcpu_ms(self) -> float:
        """Wall time the CPU thread was not running."""
        return self.wall_ms - self.cpu_ms

    def to_json(self) -> dict:
        return {"wall_ms": self.wall_ms, "cpu_ms": self.cpu_ms, "offcpu_ms": self.offcpu_ms,
                "compile_wall_ms": self.compile_wall_ms, "compile_cpu_ms": self.compile_cpu_ms,
                "noncompile_cpu_ms": self.noncompile_cpu_ms, "noncompile_wall_ms": self.noncompile_wall_ms}


def load(path: str) -> dict:
    with open(path) as file:
        trace = json.load(file)
    if trace.get("schema") != "op1.trace.v2":
        raise TraceError(f"{path}: not an op1.trace.v2 trace")
    if trace.get("dropped"):
        # A trace with a missing tail misstates compile time; never use it.
        raise TraceError(f"{path}: {trace['dropped']} events dropped")
    return trace


def marks(trace: dict) -> Dict[str, dict]:
    return {e["name"]: e for e in trace["traceEvents"] if e.get("cat") == "mark"}


def _union(spans: List[Tuple[float, float]]) -> float:
    total, end = 0.0, float("-inf")
    for begin, finish in sorted(spans):
        if finish <= end:
            continue
        total += finish - max(begin, end)
        end = finish
    return total


def phase(trace: dict, begin: str, end: str) -> Phase:
    found = marks(trace)
    for name in (begin, end):
        if name not in found:
            raise TraceError(f"mark {name!r} is not in the trace")
    a, b = found[begin], found[end]
    lo, hi = a["ts"], b["ts"]
    if hi < lo:
        raise TraceError(f"mark {end!r} precedes {begin!r}")
    spans, compile_cpu_ns = [], 0
    for event in trace["traceEvents"]:
        if event.get("name") != "translate" or event.get("ph") != "X":
            continue
        start, finish = event["ts"], event["ts"] + event["dur"]
        if finish <= lo or start >= hi:
            continue
        if start < lo or finish > hi:
            # Marks fire between blocks and translation happens inside a run,
            # so a straddling span means the trace is not what this expects.
            raise TraceError(f"translate span at {start} straddles {begin}..{end}")
        spans.append((start, finish))
        compile_cpu_ns += event["args"]["cpu_ns"]
    return Phase(
        wall_ms=(b["args"]["wall_ns"] - a["args"]["wall_ns"]) / 1e6,
        cpu_ms=(b["args"]["thread_cpu_ns"] - a["args"]["thread_cpu_ns"]) / 1e6,
        compile_wall_ms=_union(spans) / 1e3,
        compile_cpu_ms=compile_cpu_ns / 1e6,
    )
