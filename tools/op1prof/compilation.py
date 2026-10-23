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
    # bcore's Core::set_tiering: 0 a tier-0 block, 1 its promotion; None untiered.
    tier: Optional[int] = None
    # A background promotion (Core::set_tier_background) is two spans on the
    # CPU thread: "lift" (no code yet) and "link" (the worker's code loaded).
    part: Optional[str] = None
    # bcore's Core::set_copy_patch: made of stencils; of its packets, those
    # whose stencils were built at run time (Core::set_copy_patch_runtime),
    # and the stencils this translation built (also for a block LLVM then
    # compiled, as a packet had none).
    copy_patch: bool = False
    cp_runtime_packets: int = 0
    cp_runtime_built: int = 0


def translations(trace_data: dict, window: Optional[Tuple[str, str]] = None) -> List[Translation]:
    lo, hi = _bounds(trace_data, window)
    out = []
    for e in trace_data["traceEvents"]:
        if e.get("name") != "translate" or e.get("ph") != "X" or not lo <= e["ts"] < hi:
            continue
        args = e["args"]
        out.append(Translation(pc=args["pc"], begin_us=e["ts"], cpu_ns=args["cpu_ns"], ok=args.get("ok", True),
                               ir_insns=args.get("ir_insns"), object_bytes=args.get("object_bytes"),
                               object_hash=args.get("object_hash"), tier=args.get("tier"),
                               part=args.get("part"), copy_patch=bool(args.get("copy_patch")),
                               cp_runtime_packets=args.get("cp_runtime_packets", 0),
                               cp_runtime_built=args.get("cp_runtime_built", 0)))
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


UNKNOWN_HASH = "0" * 16  # bcore's 0: no code map, the hash could not be computed


def has_shape(spans: List[Translation]) -> bool:
    """Every successful translation carries a known code hash, and there is
    at least one: anything less cannot vouch for what was compiled."""
    compiled = [t for t in spans if t.ok and t.part != "lift"]
    return bool(compiled) and all(t.object_hash not in (None, UNKNOWN_HASH) for t in compiled)


def codegen(spans: List[Translation]) -> dict:
    """What was compiled, independent of host addresses and timing: one
    digest over every distinct (pc, code hash). Two configurations with the
    same digest generated the same machine code for the same blocks."""
    compiled = sorted({(t.pc, t.object_hash) for t in spans if t.ok and t.object_hash and t.part != "lift"})
    digest = hashlib.sha256("".join(f"{pc:08x}:{h}\n" for pc, h in compiled).encode()).hexdigest()
    made = [t for t in spans if t.ok and t.part != "lift"]
    return {"translations": len(made), "distinct_code": len(compiled),
            "ir_insns": sum(t.ir_insns or 0 for t in made),
            "object_bytes": sum(t.object_bytes or 0 for t in made),
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
    # A promotion (tier 1) compiles a block again on purpose: not counted here.
    again = {pc: [t for t in ts if t.tier != 1] for pc, ts in by_pc.items()}
    again = {pc: ts for pc, ts in again.items() if len(ts) > 1}
    again_ms = sum(t.cpu_ns for ts in again.values() for t in ts[1:]) / 1e6
    failed = sum(1 for t in spans if not t.ok)
    lines = [f"compile {title}: {len(spans)} translations of {len(by_pc)} blocks, {sum(cpu_ms) / 1e3:.3f} s CPU"
             + (f", {failed} failed" if failed else ""),
             f"per translation: p50 {_percentile(cpu_ms, 0.5):.2f} ms, p95 {_percentile(cpu_ms, 0.95):.2f} ms, "
             f"max {max(cpu_ms):.2f} ms",
             f"translated again: {len(again)} blocks, {sum(len(ts) - 1 for ts in again.values())} extra "
             f"translations, {again_ms / 1e3:.3f} s CPU (the cache is flushed when the boot ROM is re-entered)"]
    tiers = defaultdict(list)
    for t in spans:
        if t.tier is not None and t.part != "lift":  # a background lift is no translation of its own
            tiers[t.tier].append(t)
    bg = [e for e in trace_data["traceEvents"] if e.get("name") == trace.BG_SPAN and e.get("ph") == "X"
          and _bounds(trace_data, window)[0] <= e["ts"] < _bounds(trace_data, window)[1]]
    if bg:
        lifts = [t for t in spans if t.part == "lift"]
        discarded = sum(1 for e in bg if e["args"].get("discarded"))
        lines.append(f"background: {len(bg)} promotions on the worker, {sum(e['args']['cpu_ns'] for e in bg) / 1e9:.3f} s "
                     f"of its CPU ({discarded} discarded); on this thread {len(lifts)} lifts, "
                     f"{sum(t.cpu_ns for t in lifts) / 1e9:.3f} s, and their loads below")
    if tiers:
        lines.append("tiered: " + "; ".join(
            f"tier {tier} {len(ts)} translations, {sum(t.cpu_ns for t in ts) / 1e9:.3f} s CPU"
            + (" (promotions)" if tier == 1 else "") for tier, ts in sorted(tiers.items())))
    stenciled = [t for t in spans if t.copy_patch]
    if stenciled:
        llvm0 = [t for t in tiers.get(0, []) if not t.copy_patch]
        runtime = [t for t in stenciled if t.cp_runtime_packets]
        built = [t for t in spans if t.cp_runtime_built]
        lines.append(f"copy-and-patch: {len(stenciled)} blocks of stencils, {sum(t.cpu_ns for t in stenciled) / 1e9:.3f} s "
                     f"CPU ({len(runtime)} with stencils built at run time; {sum(t.cp_runtime_built for t in built)} "
                     f"stencils built in {len(built)} of the translations, their CPU included)"
                     + (f"; tier 0 with LLVM {len(llvm0)} blocks, {sum(t.cpu_ns for t in llvm0) / 1e9:.3f} s" if tiers else ""))
    lo, hi = _bounds(trace_data, window)
    flushes = [e for e in trace.jit_spans(trace_data) if e["name"] == "invalidate" and lo <= e["ts"] < hi]
    if flushes:
        lines.append(f"cache flushes: {len(flushes)}, {sum(e['args']['cpu_ns'] for e in flushes) / 1e6:.2f} ms CPU "
                     "(rebuilding the JIT; part of compile time, not in the translations above)")
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
