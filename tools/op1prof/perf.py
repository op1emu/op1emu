"""perf capture of one run between two marks (reported by report.py)."""
from __future__ import annotations

import json
import os
import re
import subprocess
from dataclasses import replace
from pathlib import Path
from typing import List, Optional, Tuple

from .runner import RunResult, RunSpec, run

PRECISE = re.compile(r":[a-z]*p")


def is_precise(event: str) -> bool:
    """`cpu-clock` is a software timer: right for symbol and address-range
    totals, wrong for "which instruction". One capture put 6.74% on a single
    instruction that a precise cycles event ranked outside the top 20."""
    return bool(PRECISE.search(event))


def mark_order(marks_file: Path) -> List[str]:
    """Marks in the order a marks file lists them, the frame mark last. The
    profiler does not impose it (any armed range can fire first), so it is a
    contract on the file that check_fired() verifies after each capture."""
    config = json.loads(marks_file.read_text())
    order = ["start"] + [m["name"] for m in config["marks"]]
    if "frame" in config:
        order.append(config["frame"]["name"])
    return order


def check_window(marks_file: Path, window: Tuple[str, str], until: str) -> Optional[str]:
    order = mark_order(marks_file)
    begin, end = window
    for name in (begin, end, until):
        if name not in order:
            return f"unknown mark {name!r}"
    # An inverted window records nothing and looks merely empty, not wrong.
    if order.index(begin) >= order.index(end):
        return f"window {begin}:{end} is empty: {begin} does not come before {end}"
    # Stopping first would leave the window open and the end mark missing.
    if order.index(until) < order.index(end):
        return f"--until {until} comes before the window end {end}"
    return None


def check_fired(marks_file: Path, window: Tuple[str, str], fired: List[str]) -> Optional[str]:
    """After a capture: the marks fired in file order, which check_window()
    assumed, and both window marks fired, so the window opened and closed."""
    order = mark_order(marks_file)
    seen = [name for name in fired if name in order]
    if seen != sorted(seen, key=order.index):
        return f"marks fired out of file order ({seen}); list them in the order they fire"
    missing = [name for name in window if name not in fired]
    if missing:
        return f"window marks did not fire: {missing} (fired: {fired})"
    return None


SOFTWARE_EVENTS = ("cpu-clock", "task-clock")


def is_software_event(event: str) -> bool:
    """A software timer event (`cpu-clock`, `task-clock:u`): its sample
    period is CPU nanoseconds, and it has no branch records."""
    return event.split(":")[0] in SOFTWARE_EVENTS


def check_call_graph(event: str, call_graph: str) -> Optional[str]:
    """`fp` walks frame pointers, which generated code and system libraries
    without them break (on one capture 11.5% of execution weight never
    reached the CPU loop). `lbr` takes the call stack from Intel's last
    branch records, independent of frame pointers, but only for a hardware
    event."""
    if call_graph == "lbr" and is_software_event(event):
        return f"--call-graph lbr needs a hardware event, not {event} (try --event cycles:u --period 4000000)"
    return None


def preflight(event: str, rate: List[str], call_graph: str = "fp") -> Optional[str]:
    try:
        paranoid = int(Path("/proc/sys/kernel/perf_event_paranoid").read_text())
    except (OSError, ValueError):
        paranoid = None
    probe = subprocess.run(["perf", "record", "-q", "-e", event, *rate, "--call-graph", call_graph,
                            "-o", "/dev/null", "--", "true"],
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
           period: Optional[int], freq: Optional[int], call_graph: str = "fp") -> RunResult:
    """Run under perf with sampling enabled only between two marks."""
    rate = ["-c", str(period)] if period else ["-F", str(freq or 999)]
    directory.mkdir(parents=True, exist_ok=False)
    fifo = directory / "perf.ctl"
    os.mkfifo(fifo)
    # No ack FIFO: an ack pipe nobody reads deadlocks perf.
    prefix = ["perf", "record", "-q", "-k", "mono", "--call-graph", call_graph, "--delay=-1", f"--control=fifo:{fifo}",
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
        {"event": event, "precise": is_precise(event), "rate": rate, "call_graph": call_graph,
         "window": list(window),
         "run": result.to_json()}, indent=1))
    return result
