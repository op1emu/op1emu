"""Compilation report: what the JIT compiled in a phase, and what it cost."""
from __future__ import annotations

import hashlib
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

from . import trace
from .census import Symbols

STAGES = ("lift", "ir-optimize", "materialize")


@dataclass(frozen=True)
class Translation:
    pc: int
    begin_us: float
    cpu_ns: int
    ok: bool
    ir_insns: Optional[int]
    object_bytes: Optional[int]
    object_hash: Optional[str]


def translations(trace_data: dict, window: Optional[Tuple[str, str]] = None) -> List[Translation]:
    lo, hi = _bounds(trace_data, window)
    out = []
    for e in trace_data["traceEvents"]:
        if e.get("name") != "translate" or e.get("ph") != "X" or not lo <= e["ts"] < hi:
            continue
        args = e["args"]
        out.append(Translation(pc=args["pc"], begin_us=e["ts"], cpu_ns=args["cpu_ns"], ok=args.get("ok", True),
                               ir_insns=args.get("ir_insns"), object_bytes=args.get("object_bytes"),
                               object_hash=args.get("object_hash")))
    return out


def _bounds(trace_data: dict, window: Optional[Tuple[str, str]]) -> Tuple[float, float]:
    if not window:
        return float("-inf"), float("inf")
    found = trace.marks(trace_data)
    for name in window:
        if name not in found:
            raise trace.TraceError(f"mark {name!r} is not in the trace")
    return found[window[0]]["ts"], found[window[1]]["ts"]


def stage_ms(trace_data: dict, window: Optional[Tuple[str, str]] = None) -> Dict[str, float]:
    """Wall time per stage. Stages do not cover a whole translation, so they
    show proportions; their sum is not compile time."""
    lo, hi = _bounds(trace_data, window)
    totals = defaultdict(float)
    for e in trace_data["traceEvents"]:
        if e.get("name") in STAGES and e.get("ph") == "X" and lo <= e["ts"] < hi:
            totals[e["name"]] += e["dur"] / 1e3
    return dict(totals)


def has_shape(spans: List[Translation]) -> bool:
    return bool(spans) and all(t.object_hash is not None for t in spans if t.ok)


def codegen(spans: List[Translation]) -> dict:
    """What was compiled, independent of host addresses and timing: one
    digest over every distinct (pc, code hash). Two configurations with the
    same digest generated the same machine code for the same blocks."""
    compiled = sorted({(t.pc, t.object_hash) for t in spans if t.ok and t.object_hash})
    digest = hashlib.sha256("".join(f"{pc:08x}:{h}\n" for pc, h in compiled).encode()).hexdigest()
    return {"translations": len(spans), "distinct_code": len(compiled),
            "ir_insns": sum(t.ir_insns or 0 for t in spans if t.ok),
            "object_bytes": sum(t.object_bytes or 0 for t in spans if t.ok),
            "digest": digest}


def _percentile(values: List[float], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def report(trace_data: dict, window: Optional[Tuple[str, str]], limit: int, symbols: Symbols) -> str:
    spans = translations(trace_data, window)
    title = f"{window[0]} -> {window[1]}" if window else "whole run"
    if not spans:
        return f"compile {title}: no translations"
    cpu_ms = [t.cpu_ns / 1e6 for t in spans]
    by_pc: Dict[int, List[Translation]] = defaultdict(list)
    for t in spans:
        by_pc[t.pc].append(t)
    again = {pc: ts for pc, ts in by_pc.items() if len(ts) > 1}
    again_ms = sum(t.cpu_ns for ts in again.values() for t in ts[1:]) / 1e6
    failed = sum(1 for t in spans if not t.ok)
    lines = [f"compile {title}: {len(spans)} translations of {len(by_pc)} blocks, {sum(cpu_ms) / 1e3:.3f} s CPU"
             + (f", {failed} failed" if failed else ""),
             f"per translation: p50 {_percentile(cpu_ms, 0.5):.2f} ms, p95 {_percentile(cpu_ms, 0.95):.2f} ms, "
             f"max {max(cpu_ms):.2f} ms",
             f"translated again: {len(again)} blocks, {sum(len(ts) - 1 for ts in again.values())} extra "
             f"translations, {again_ms / 1e3:.3f} s CPU (the cache is flushed when the boot ROM is re-entered)"]
    stages = stage_ms(trace_data, window)
    if stages:
        total = sum(stages.values())
        lines.append("stages (wall, proportions only; they do not add up to compile time): "
                     + ", ".join(f"{name} {stages[name] / total * 100:.0f}%" for name in STAGES if name in stages))
    if has_shape(spans):
        shape = codegen(spans)
        lines.append(f"compiled: {shape['ir_insns']:,} IR instructions, {shape['object_bytes']:,} bytes of "
                     f"machine code, {shape['distinct_code']} distinct blocks of code (digest {shape['digest'][:16]})")
    cost = sorted(by_pc.items(), key=lambda kv: -sum(t.cpu_ns for t in kv[1]))
    shaped = has_shape(spans)
    lines += ["", f"{'block':>10} {'times':>5} {'cpu ms':>9} {'ms/each':>8}"
              + (f" {'IR':>7} {'bytes':>7} {'us/IR':>6}" if shaped else "") + "  symbol"]
    for pc, ts in cost[:limit]:
        total_ms = sum(t.cpu_ns for t in ts) / 1e6
        row = f"0x{pc:08x} {len(ts):>5} {total_ms:9.2f} {total_ms / len(ts):8.2f}"
        if shaped:
            ir = ts[-1].ir_insns or 0
            row += f" {ir:>7} {ts[-1].object_bytes or 0:>7} {total_ms / len(ts) * 1e3 / ir if ir else 0:6.2f}"
        lines.append(row + f"  {symbols.name(pc)}")
    return "\n".join(lines)
