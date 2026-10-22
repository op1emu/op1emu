"""The emulator's op1.trace.v2/v3/v4 trace and the metrics derived from it.

v3 adds per-mark process CPU, interrupt entries and, with
--profile-counters, the CPU thread's user-space instructions and cycles
(also inside every translate span). v4 adds `invalidate` spans (bcore's
cache flush at a boot ROM entry, which rebuilds the JIT), the binary's
build identity and the recorder's capacity."""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

SCHEMAS = ("op1.trace.v2", "op1.trace.v3", "op1.trace.v4")
# JIT work: compiling a block, and flushing the cache (which destroys the
# compiled code and builds a new JIT). Everything outside them is execution.
JIT_SPANS = ("translate", "invalidate")
# A promotion's IR pipeline and backend on bcore's worker thread
# (Core::set_tier_background): JIT work too, but on another thread, so never
# part of the CPU thread's time.
BG_SPAN = "translate-bg"


class TraceError(ValueError):
    pass


@dataclass(frozen=True)
class Phase:
    """Time between two marks, split into compilation and everything else."""

    wall_ms: float          # monotonic time between the marks
    cpu_ms: float           # the CPU thread's CPU time between them
    compile_wall_ms: float  # union of JIT spans (translate, invalidate) inside the window
    compile_cpu_ms: float   # CPU time inside those spans
    translations: int = 0   # translate spans inside the window
    invalidations: int = 0  # invalidate spans inside the window (v4)
    invalidate_cpu_ms: float = 0.0  # their part of compile_cpu_ms
    process_cpu_ms: Optional[float] = None  # every thread of the process (v3)
    insns: Optional[int] = None             # CPU thread's user-space instructions (--profile-counters)
    compile_insns: Optional[int] = None     # instructions inside translate spans
    host_cycles: Optional[int] = None
    bg_compile_cpu_ms: float = 0.0  # the JIT worker's CPU in the window (bg_cpu_ms)
    bg_translations: int = 0
    bg_exact: bool = True  # False: bg_compile_cpu_ms is an estimate (bg_cpu_ms)

    @property
    def jit_cpu_ms(self) -> float:
        """All JIT CPU: the CPU thread's compilation and the worker's."""
        return self.compile_cpu_ms + self.bg_compile_cpu_ms

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

    @property
    def other_threads_cpu_ms(self) -> Optional[float]:
        """CPU of the process's other threads (frontend, USB/IP loop): they
        can compete with the CPU thread for its core. Unknown where the JIT
        worker's share is only an estimate."""
        if self.process_cpu_ms is None or not self.bg_exact:
            return None
        return self.process_cpu_ms - self.cpu_ms - self.bg_compile_cpu_ms

    @property
    def noncompile_insns(self) -> Optional[int]:
        """Execution instructions, split from compilation like noncompile_cpu_ms."""
        return None if self.insns is None or self.compile_insns is None else self.insns - self.compile_insns

    def to_json(self) -> dict:
        return {"wall_ms": self.wall_ms, "cpu_ms": self.cpu_ms, "offcpu_ms": self.offcpu_ms,
                "compile_wall_ms": self.compile_wall_ms, "compile_cpu_ms": self.compile_cpu_ms,
                "noncompile_cpu_ms": self.noncompile_cpu_ms, "noncompile_wall_ms": self.noncompile_wall_ms,
                "translations": self.translations, "invalidations": self.invalidations,
                "invalidate_cpu_ms": self.invalidate_cpu_ms, "process_cpu_ms": self.process_cpu_ms,
                "other_threads_cpu_ms": self.other_threads_cpu_ms, "insns": self.insns,
                "compile_insns": self.compile_insns, "noncompile_insns": self.noncompile_insns,
                "host_cycles": self.host_cycles, "bg_compile_cpu_ms": self.bg_compile_cpu_ms,
                "bg_translations": self.bg_translations, "bg_exact": self.bg_exact, "jit_cpu_ms": self.jit_cpu_ms}


def load(path: str) -> dict:
    with open(path) as file:
        trace = json.load(file)
    if trace.get("schema") not in SCHEMAS:
        raise TraceError(f"{path}: not an op1 trace ({' or '.join(SCHEMAS)})")
    if trace.get("dropped"):
        # A trace with a missing tail misstates compile time; never use it.
        raise TraceError(f"{path}: {trace['dropped']} events dropped")
    if trace.get("deterministic") is False:
        # Host time: the guest did whatever the host's speed let it do, so
        # nothing in this trace repeats or compares.
        raise TraceError(f"{path}: recorded without --deterministic")
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


def jit_spans(trace: dict) -> List[dict]:
    return [e for e in trace["traceEvents"] if e.get("name") in JIT_SPANS and e.get("ph") == "X"]


def phase(trace: dict, begin: str, end: str) -> Phase:
    found = marks(trace)
    for name in (begin, end):
        if name not in found:
            raise TraceError(f"mark {name!r} is not in the trace")
    a, b = found[begin], found[end]
    lo, hi = a["ts"], b["ts"]
    if hi < lo:
        raise TraceError(f"mark {end!r} precedes {begin!r}")
    spans, compile_cpu_ns, compile_insns, translations, invalidations, invalidate_cpu_ns = [], 0, 0, 0, 0, 0
    counted = bool(trace.get("counters"))
    for event in jit_spans(trace):
        start, finish = event["ts"], event["ts"] + event["dur"]
        if finish <= lo or start >= hi:
            continue
        if start < lo or finish > hi:
            # Marks fire between blocks and translation happens inside a run,
            # so a straddling span means the trace is not what this expects.
            raise TraceError(f"{event['name']} span at {start} straddles {begin}..{end}")
        spans.append((start, finish))
        compile_cpu_ns += event["args"]["cpu_ns"]
        if event["name"] == "translate":
            # A background promotion's lift (part "lift") has no code of its own.
            translations += event["args"].get("part") != "lift"
        else:
            invalidations += 1
            invalidate_cpu_ns += event["args"]["cpu_ns"]
        if counted:
            compile_insns += event["args"]["insns"]
    bg = [e for e in trace["traceEvents"] if e.get("name") == BG_SPAN and e.get("ph") == "X" and lo <= e["ts"] < hi]
    bg_ms, bg_exact = bg_cpu_ms(trace, a, b)
    return Phase(
        bg_compile_cpu_ms=bg_ms,
        bg_exact=bg_exact,
        bg_translations=len(bg),
        wall_ms=(b["args"]["wall_ns"] - a["args"]["wall_ns"]) / 1e6,
        cpu_ms=(b["args"]["thread_cpu_ns"] - a["args"]["thread_cpu_ns"]) / 1e6,
        compile_wall_ms=_union(spans) / 1e3,
        compile_cpu_ms=compile_cpu_ns / 1e6,
        translations=translations,
        invalidations=invalidations,
        invalidate_cpu_ms=invalidate_cpu_ns / 1e6,
        process_cpu_ms=_delta(a, b, "process_cpu_ns", 1e6),
        insns=_delta(a, b, "host_insns") if counted else None,
        compile_insns=compile_insns if counted else None,
        host_cycles=_delta(a, b, "host_cycles") if counted else None,
    )


def bg_cpu_ms(trace: dict, a: dict, b: dict) -> Tuple[float, bool]:
    """The JIT worker's CPU between two marks, and whether it is exact: the
    difference of its thread CPU time at them, which the emulator records at
    every mark. Without it, the CPU of the translate-bg spans in the phase:
    exact where each lies wholly inside it; where one crosses a mark, its CPU
    split by how much of the span lies in the phase, an estimate (the worker
    need not have run evenly through it)."""
    if "jit_worker_cpu_ns" in a["args"] and "jit_worker_cpu_ns" in b["args"]:
        return max(0, b["args"]["jit_worker_cpu_ns"] - a["args"]["jit_worker_cpu_ns"]) / 1e6, True
    lo, hi = a["ts"], b["ts"]
    total, exact = 0.0, True
    for e in trace["traceEvents"]:
        if e.get("name") != BG_SPAN or e.get("ph") != "X":
            continue
        start, finish = e["ts"], e["ts"] + e["dur"]
        if finish <= lo or start >= hi:
            continue
        if start < lo or finish > hi:
            exact = False
        share = 1.0 if finish <= start else (min(finish, hi) - max(start, lo)) / (finish - start)
        total += e["args"]["cpu_ns"] * share
    return total / 1e6, exact


def _delta(a: dict, b: dict, key: str, scale: Optional[float] = None):
    if key not in a["args"] or key not in b["args"]:
        return None
    value = b["args"][key] - a["args"][key]
    return value / scale if scale else value


def phases(trace: dict) -> List[Tuple[str, str, Phase]]:
    """Every pair of consecutive fired marks, in firing order."""
    order = [e["name"] for e in sorted(marks(trace).values(), key=lambda e: e["ts"])]
    return [(a, b, phase(trace, a, b)) for a, b in zip(order, order[1:])]
