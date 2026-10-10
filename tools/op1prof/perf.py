"""perf capture of one run, and a report attributed by guest block."""
from __future__ import annotations

import bisect
import json
import os
import re
import subprocess
from collections import Counter
from dataclasses import replace
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

from . import trace
from .runner import RunResult, RunSpec, run

PRECISE = re.compile(r":[a-z]*p")


def is_precise(event: str) -> bool:
    """`cpu-clock` is a software timer: right for symbol and address-range
    totals, wrong for "which instruction". One capture put 6.74% on a single
    instruction that a precise cycles event ranked outside the top 20."""
    return bool(PRECISE.search(event))


def mark_order(marks_file: Path) -> List[str]:
    config = json.loads(marks_file.read_text())
    order = ["start"] + [m["name"] for m in config["marks"]]
    if "frame" in config:
        order.append(config["frame"]["name"])
    return order


def check_window(marks_file: Path, window: Tuple[str, str]) -> Optional[str]:
    order = mark_order(marks_file)
    begin, end = window
    for name in window:
        if name not in order:
            return f"unknown mark {name!r}"
    # An inverted window records nothing and looks merely empty, not wrong.
    if order.index(begin) >= order.index(end):
        return f"window {begin}:{end} is empty: {begin} does not come before {end}"
    return None


def preflight(event: str, rate: List[str]) -> Optional[str]:
    try:
        paranoid = int(Path("/proc/sys/kernel/perf_event_paranoid").read_text())
    except (OSError, ValueError):
        paranoid = None
    probe = subprocess.run(["perf", "record", "-q", "-e", event, *rate, "-o", "/dev/null", "--", "true"],
                           capture_output=True, text=True)
    if probe.returncode == 0:
        return None
    message = f"perf cannot record {event} {' '.join(rate)}: {probe.stderr.strip()[-300:]}"
    if paranoid is not None and paranoid > 2:
        # Above 2 is a Debian/Ubuntu extension that blocks unprivileged
        # perf_event_open() altogether; 2 allows user-space events of one's
        # own processes, which is all these captures need.
        message += (f"\nkernel.perf_event_paranoid is {paranoid}: no unprivileged profiling at all. This tool "
                    "never changes it; for user-space events: sudo sysctl kernel.perf_event_paranoid=2 "
                    f"(restore {paranoid} afterwards)")
    elif paranoid == 2 and "/" not in event:
        name, _, modifiers = event.partition(":")
        if "u" not in modifiers:  # no modifier at all also counts the kernel
            message += ("\nkernel.perf_event_paranoid is 2, which allows user-space events only: "
                        f"add the u modifier (e.g. {name}:u{modifiers})")
    return message


def record(spec: RunSpec, directory: Path, window: Tuple[str, str], event: str,
           period: Optional[int], freq: Optional[int]) -> RunResult:
    """Run under perf with sampling enabled only between two marks."""
    rate = ["-c", str(period)] if period else ["-F", str(freq or 999)]
    directory.mkdir(parents=True, exist_ok=False)
    fifo = directory / "perf.ctl"
    os.mkfifo(fifo)
    # No ack FIFO: an ack pipe nobody reads deadlocks perf.
    prefix = ["perf", "record", "-q", "-k", "mono", "-g", "--delay=-1", f"--control=fifo:{fifo}",
              "-e", event, *rate, "-o", str(directory / "perf.data"), "--"]
    perf_spec = replace(spec, prefix=prefix, env={**spec.env, "JITDUMPDIR": str(directory / "run")},
                        args=spec.args + ["--perf-ctl-fifo", str(fifo), "--perf-window",
                                          f"{window[0]}:{window[1]}", "--perf-jitdump"])
    result = run(perf_spec, directory / "run")
    if result.reached:
        inject = subprocess.run(["perf", "inject", "--jit", "-i", str(directory / "perf.data"),
                                 "-o", str(directory / "perf.jit.data")], capture_output=True, text=True)
        if inject.returncode != 0:
            result.error = f"perf inject failed: {inject.stderr.strip()[-300:]}"
    (directory / "capture.json").write_text(json.dumps(
        {"event": event, "precise": is_precise(event), "rate": rate, "window": list(window),
         "run": result.to_json()}, indent=1))
    return result


SAMPLE = re.compile(r"^\s*(?P<tid>\d+)\s+(?P<time>\d+\.\d+):\s+(?P<ip>[0-9a-f]+)\s+(?P<sym>.*?)\s+\((?P<dso>[^)]*)\)\s*$")


def parse_samples(lines: Iterable[str]) -> Iterable[Tuple[int, int, str, str]]:
    """(tid, time_ns, symbol, dso) from `perf script -F tid,time,ip,sym,dso -G --ns`."""
    for line in lines:
        match = SAMPLE.match(line)
        if match:
            seconds, _, fraction = match.group("time").partition(".")
            time_ns = int(seconds) * 1_000_000_000 + int(fraction.ljust(9, "0")[:9])
            yield int(match.group("tid")), time_ns, match.group("sym"), os.path.basename(match.group("dso"))


def attribute(samples, trace_data: dict, window: Tuple[str, str]) -> dict:
    """Leaf samples of the CPU thread inside the window, outside translation."""
    found = trace.marks(trace_data)
    lo, hi = (int(found[name]["ts"] * 1000) for name in window)
    spans = sorted((int(e["ts"] * 1000), int((e["ts"] + e["dur"]) * 1000))
                   for e in trace_data["traceEvents"] if e.get("name") == "translate")
    starts = [s for s, _ in spans]
    tid = trace_data.get("cpu_thread_tid")
    by_block, by_symbol = Counter(), Counter()
    counts = Counter()
    for sample_tid, time_ns, symbol, dso in samples:
        if tid is not None and sample_tid != tid:
            counts["other_threads"] += 1
            continue
        if not lo <= time_ns < hi:
            counts["outside_window"] += 1
            continue
        index = bisect.bisect_right(starts, time_ns) - 1
        if index >= 0 and time_ns < spans[index][1]:
            counts["compile"] += 1
            continue
        counts["execution"] += 1
        if symbol.startswith("bb_0x"):
            by_block[symbol] += 1
        else:
            by_symbol[f"{symbol} ({dso})"] += 1
    return {"counts": dict(counts), "blocks": by_block, "symbols": by_symbol}


def report(capture: Path, limit: int) -> str:
    info = json.loads((capture / "capture.json").read_text())
    window = tuple(info["window"])
    trace_data = trace.load(str(capture / "run" / "trace.json"))
    script = subprocess.run(["perf", "script", "-i", str(capture / "perf.jit.data"), "-F", "tid,time,ip,sym,dso",
                             "-G", "--ns"], capture_output=True, text=True)
    if script.returncode != 0:
        raise RuntimeError(f"perf script failed: {script.stderr.strip()[-300:]}")
    result = attribute(parse_samples(script.stdout.splitlines()), trace_data, window)
    execution = result["counts"].get("execution", 0)
    jit = sum(result["blocks"].values())
    phase = trace.phase(trace_data, *window)
    lines = [f"perf {info['event']} {' '.join(info['rate'])}, window {window[0]} -> {window[1]}",
             f"samples: {result['counts']}",
             f"execution samples: {execution}; generated code {jit / execution * 100 if execution else 0:.1f}%, "
             f"host {100 - (jit / execution * 100 if execution else 0):.1f}%",
             f"trace: execution CPU {phase.noncompile_cpu_ms / 1e3:.3f} s, compile CPU {phase.compile_cpu_ms / 1e3:.3f} s",
             "Shares are where samples landed, not what removing that code would save."]
    if not info["precise"]:
        lines.append(f"{info['event']} is not a precise event: use these totals by block and symbol, "
                     "never to rank single instructions (use cycles:upp for that).")
    lines += ["", f"{'guest block':16s} {'samples':>8} {'share':>6}"]
    for block, count in result["blocks"].most_common(limit):
        lines.append(f"{block:16s} {count:>8} {count / execution * 100:5.1f}%")
    lines += ["", f"{'host symbol':60s} {'samples':>8} {'share':>6}"]
    for symbol, count in result["symbols"].most_common(limit):
        lines.append(f"{symbol[:60]:60s} {count:>8} {count / execution * 100:5.1f}%")
    return "\n".join(lines)
