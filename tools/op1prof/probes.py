"""Guest probes (--profile-probes) and the thread timeline built from them."""
from __future__ import annotations

import json
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from . import trace

# Guest cycles per microsecond: the emulated core clock (400 MHz).
CYCLES_PER_US = 400


@dataclass(frozen=True)
class Hit:
    probe: str
    cycles: int
    runs: int
    packets: int
    values: Dict[str, object]   # register or memory read name -> value (None when not read)


def hits(trace_data: dict, names: Optional[Sequence[str]] = None) -> List[Hit]:
    out = []
    for event in trace_data["traceEvents"]:
        if event.get("cat") != "probe" or (names and event["name"] not in names):
            continue
        args = dict(event["args"])
        base = {key: args.pop(key) for key in ("pc", "runs", "packets", "cycles")}
        out.append(Hit(event["name"], base["cycles"], base["runs"], base["packets"], args))
    return sorted(out, key=lambda h: (h.cycles, h.runs))


def reads(trace_data: dict) -> List[dict]:
    return [{"mark": e["args"]["mark"], "name": e["name"], "addr": e["args"]["addr"], "words": e["args"]["words"]}
            for e in trace_data["traceEvents"] if e.get("cat") == "read"]


def _value(value) -> str:
    if value is None:
        return "-"
    if isinstance(value, list):
        return "[" + ",".join(_value(v) for v in value) + "]"
    return f"0x{value:x}"


def summary(trace_data: dict, limit: int) -> str:
    """Per probe: hits, and the most frequent values of each captured field."""
    by_probe: Dict[str, List[Hit]] = defaultdict(list)
    for h in hits(trace_data):
        by_probe[h.probe].append(h)
    lines = []
    for name, probe_hits in by_probe.items():
        lost = trace_data.get("probes", {}).get(name, {}).get("dropped", 0)
        lines.append(f"{name}: {len(probe_hits)} hits, guest cycles {probe_hits[0].cycles:,} .. "
                     f"{probe_hits[-1].cycles:,}" + (f"; {lost} more dropped past its limit" if lost else ""))
        for field in probe_hits[0].values:
            common = Counter(_value(h.values[field]) for h in probe_hits).most_common(limit)
            lines.append(f"  {field:12s} " + ", ".join(f"{v} x{n}" for v, n in common))
    for r in reads(trace_data):
        lines.append(f"read {r['name']} at {r['mark']} (0x{r['addr']:08x}): "
                     + ",".join("-" if w is None else str(w) for w in r["words"]))
    return "\n".join(lines) if lines else "no probe hits or reads in this trace (run with --probes)"


def dropped(trace_data: dict, probes: Sequence[str]) -> Dict[str, int]:
    stats = trace_data.get("probes", {})
    return {name: stats[name]["dropped"] for name in probes if stats.get(name, {}).get("dropped")}


def timeline(trace_data: dict, probes: Sequence[str], field: str) -> List[Tuple[int, int]]:
    """(guest cycles, thread) at every switch: `field` names the thread that
    runs from then on (for example the register holding the new TCB). A
    switch probe that dropped hits would silently charge the rest of the run
    to the last thread it recorded, so that is an error."""
    declared = trace_data.get("probes", {})
    unknown = [name for name in probes if name not in declared]
    if unknown:
        # A misspelled switch probe would silently drop its switches.
        raise ValueError(f"probes not declared in this trace: {', '.join(unknown)}"
                         + (f" (declared: {', '.join(declared)})" if declared else
                            " (it records no probes: run with --probes)"))
    lost = dropped(trace_data, probes)
    if lost:
        raise ValueError("switch probes dropped hits, so the timeline would be wrong: "
                         + ", ".join(f"{name} {count}" for name, count in lost.items()) + " (raise their limit)")
    switches = []
    for h in hits(trace_data, probes):
        value = h.values.get(field)
        if not isinstance(value, int):
            raise ValueError(f"probe {h.probe} has no captured value {field!r} (captured: {sorted(h.values)})")
        switches.append((h.cycles, value))
    if not switches:
        raise ValueError(f"no hits of {', '.join(probes)}: run with --probes and a marks file that declares them")
    return switches


def thread_time(switches: List[Tuple[int, int]], lo: int, hi: int) -> Dict[int, int]:
    """Guest cycles each thread held the core within [lo, hi). Before the
    first switch the thread is unknown (key -1)."""
    totals: Dict[int, int] = defaultdict(int)
    current, since = -1, lo
    for cycles, thread in switches:
        if cycles >= hi:
            break
        if cycles > since:
            totals[current] += cycles - since
            since = cycles
        current = thread
    if hi > since:
        totals[current] += hi - since
    return dict(totals)


def _label(thread: int, names: Dict[int, str]) -> str:
    return "(before the first switch)" if thread == -1 else names.get(thread, f"0x{thread:x}")


def threads_report(trace_data: dict, probes: Sequence[str], field: str, names: Dict[int, str]) -> str:
    switches = timeline(trace_data, probes, field)
    marks = sorted(trace.marks(trace_data).values(), key=lambda e: e["ts"])
    lines = [f"{len(switches)} switches ({', '.join(probes)}), thread = {field}; guest time at "
             f"{CYCLES_PER_US} cycles/us. Time in a thread includes the interrupts taken while it ran."]
    for a, b in zip(marks, marks[1:]):
        lo, hi = a["args"]["cycles"], b["args"]["cycles"]
        if hi <= lo:
            continue
        lines.append(f"\n{a['name']} -> {b['name']}: {(hi - lo) / CYCLES_PER_US / 1e3:.1f} ms guest time")
        for thread, cycles in sorted(thread_time(switches, lo, hi).items(), key=lambda kv: -kv[1]):
            lines.append(f"  {_label(thread, names):28s} {cycles / CYCLES_PER_US / 1e3:10.1f} ms "
                         f"{cycles / (hi - lo) * 100:5.1f}%")
    return "\n".join(lines)


def perfetto(trace_data: dict, probes: Sequence[str], field: str, names: Dict[int, str], path: Path) -> None:
    """A Chrome/Perfetto trace on the guest time axis: one track per thread."""
    switches = timeline(trace_data, probes, field)
    end = max(e["args"]["cycles"] for e in trace.marks(trace_data).values())
    tracks = {thread: index + 1 for index, thread in enumerate(sorted({t for _, t in switches}))}
    events = [{"name": "thread_name", "ph": "M", "pid": 1, "tid": tid, "args": {"name": _label(thread, names)}}
              for thread, tid in tracks.items()]
    for (start, thread), (stop, _) in zip(switches, switches[1:] + [(end, None)]):
        if stop > start:
            events.append({"name": _label(thread, names), "ph": "X", "pid": 1, "tid": tracks[thread],
                           "ts": start / CYCLES_PER_US, "dur": (stop - start) / CYCLES_PER_US})
    for mark in trace.marks(trace_data).values():
        events.append({"name": mark["name"], "ph": "i", "s": "g", "pid": 1, "tid": 0,
                       "ts": mark["args"]["cycles"] / CYCLES_PER_US})
    path.write_text(json.dumps({"displayTimeUnit": "ms", "traceEvents": events}))
