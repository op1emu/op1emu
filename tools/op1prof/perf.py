"""perf capture of one run between two marks (reported by report.py)."""
from __future__ import annotations

import json
import os
import re
import subprocess
from dataclasses import replace
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

from . import marks
from .runner import RunResult, RunSpec, run

PRECISE = re.compile(r":[a-z]*p")


def is_precise(event: str) -> bool:
    """`cpu-clock` is a software timer: right for symbol and address-range
    totals, wrong for "which instruction". One capture put 6.74% on a single
    instruction that a precise cycles event ranked outside the top 20."""
    return bool(PRECISE.search(event))


# Kept here for perf's callers; the checks live with the marks.
mark_order = marks.mark_order


def check_window(marks_file: Path, window: Tuple[str, str], until: str) -> Optional[str]:
    problem = marks.check_phase(marks_file, window, until)
    return problem.replace("phase", "window") if problem else None


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


def annotate(capture: Path, pc: int, allow_imprecise: bool, lines: int) -> str:
    """perf annotate of one guest block's generated code, from the code the
    capture's jitdump recorded. Instruction-level shares need a precise
    event: cpu-clock once put 6.74% on an instruction that cycles:pp ranked
    outside the top 20."""
    info = json.loads((capture / "capture.json").read_text())
    if not info["precise"] and not allow_imprecise:
        raise ValueError(f"{info['event']} is not a precise event, so it cannot rank instructions: capture with "
                         "--event cycles:upp (or pass --allow-imprecise to see it anyway)")
    symbol = f"bb_0x{pc:08x}"
    # The block's rarely run paths are a symbol of their own, which the report
    # counts as the block (report.block_pc), and so are its other versions:
    # compiled again careful after a loop-bottom guard miss, and promoted from
    # tier 0 (bcore's Core::set_tiering). Each part that has samples.
    parts, errors = [], []
    variants = [symbol + v for v in ("", ".careful", ".t1", ".careful.t1")]
    for part in [p for v in variants for p in (v, f"{v}.cold")]:
        result = subprocess.run([*perf_command(capture), "annotate", "-i", str(capture / "perf.jit.data"),
                                 "--stdio", "-s", part],
                                capture_output=True, text=True)
        if result.returncode != 0 or not result.stdout.strip():
            errors.append(result.stderr.strip()[-300:])
            continue
        text = result.stdout.splitlines()
        parts += [f"perf annotate {part} ({info['event']}, window {' -> '.join(info['window'])})"]
        parts += text[:lines] + ([f"... {len(text) - lines} more lines"] if len(text) > lines else [])
    if not parts:
        raise ValueError(f"perf annotate found no samples in {symbol} or its variants (.careful, .t1, .cold): "
                         f"{errors[0]}")
    head = []
    if not info["precise"]:
        head.append("not a precise event: shares are where the timer landed, not where the time went")
    dumps = sorted((capture / "run" / "ir").glob(f"{symbol}.*.ll")) if (capture / "run" / "ir").is_dir() else []
    head += [f"IR: {path}" for path in dumps]
    return "\n".join(head + parts)


# perf copies every file it records samples in (the binary snapshot, and the
# hundreds of thousands of jitted-*.so files of a boot that perf inject
# writes) into its build-id cache, ~/.debug by default, where they outlive
# the capture: a few rounds of boot captures filled a disk. Each capture
# keeps its cache inside itself instead, and is deleted with it.
BUILDID_DIR = "buildid"


def perf_command(capture: Path) -> List[str]:
    """`perf` with the capture's own build-id cache, when it has one (older
    captures used ~/.debug). The global --buildid-dir option, not the
    PERF_BUILDID_DIR variable, which perf 6.8 ignores."""
    cache = Path(capture) / BUILDID_DIR
    return ["perf", "--buildid-dir", str(cache.resolve())] if cache.is_dir() else ["perf"]


def preflight(event: str, rate: List[str], call_graph: str = "fp") -> Optional[str]:
    try:
        paranoid = int(Path("/proc/sys/kernel/perf_event_paranoid").read_text())
    except (OSError, ValueError):
        paranoid = None
    probe = subprocess.run(["perf", "record", "-q", "--no-buildid-cache", "-e", event, *rate,
                            "--call-graph", call_graph, "-o", "/dev/null", "--", "true"],
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


# Lost samples a report should not hide: above this share they bias every table.
MAX_LOST = 0.01
SAMPLES_LINE = re.compile(r"^\s*SAMPLE events:\s*(?P<count>\d+)")
# From `perf report -D`: the records themselves carry how many samples were
# lost; `--stats` counts records, so one record of 261 lost samples reads 1.
LOST_RECORD = re.compile(r"PERF_RECORD_LOST: .*lost:(?P<lost>\d+)")
LOST_SAMPLES_RECORD = re.compile(r"PERF_RECORD_LOST_SAMPLES: .*lost samples\s*:(?P<lost>\d+)")
THROTTLE_RECORD = re.compile(r"PERF_RECORD_THROTTLE\b")


def parse_record_stats(stats_lines: List[str], dump_lines: Iterable[str]) -> dict:
    """Samples (from `perf report --stats`), samples lost and throttling
    (perf lowering the rate because the handler took too long), from the
    records of `perf report -D`. perf writes one LOST record per overflow of
    its buffer and, at the end, one LOST_SAMPLES record per event summing
    them: count the summaries when there are any, else the overflows, never
    both."""
    samples = 0
    for line in stats_lines:
        match = SAMPLES_LINE.match(line)
        if match:
            samples = int(match.group("count"))
            break
    overflows, summaries, throttled = 0, None, 0
    for line in dump_lines:
        if (match := LOST_SAMPLES_RECORD.search(line)):
            summaries = (summaries or 0) + int(match.group("lost"))
        elif (match := LOST_RECORD.search(line)):
            overflows += int(match.group("lost"))
        elif THROTTLE_RECORD.search(line):
            throttled += 1
    lost = summaries if summaries is not None else overflows
    out = {"samples": samples, "lost": lost, "throttle": throttled, "warning": None}
    problems = []
    if samples + lost and lost / (samples + lost) > MAX_LOST:
        problems.append(f"{lost} of {samples + lost} samples lost ({lost / (samples + lost):.1%})")
    if throttled:
        problems.append(f"perf throttled the sampling {throttled} times (its rate fell below the one asked for)")
    if problems:
        out["warning"] = "; ".join(problems) + ": the tables under-sample those stretches"
    return out


def record_stats(data: Path) -> dict:
    perf = perf_command(data.parent)
    stats = subprocess.run([*perf, "report", "--stats", "-i", str(data)], capture_output=True, text=True)
    if stats.returncode != 0:
        return {"samples": None, "lost": None, "throttle": None,
                "warning": f"perf report --stats failed: {stats.stderr.strip()[-200:]}"}
    # Streamed: a raw dump of a long capture is large.
    dump = subprocess.Popen([*perf, "report", "-D", "-i", str(data)], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, errors="replace")
    try:
        out = parse_record_stats(stats.stdout.splitlines(), dump.stdout)
    finally:
        dump.stdout.close()
        dump.wait()
    if dump.returncode != 0:
        out["warning"] = (out["warning"] + "; " if out["warning"] else "") + "perf report -D failed: lost samples unknown"
    return out


def record(spec: RunSpec, directory: Path, window: Tuple[str, str], event: str,
           period: Optional[int], freq: Optional[int], call_graph: str = "fp") -> RunResult:
    """Run under perf with sampling enabled only between two marks."""
    rate = ["-c", str(period)] if period else ["-F", str(freq or 999)]
    directory.mkdir(parents=True, exist_ok=False)
    fifo = directory / "perf.ctl"
    os.mkfifo(fifo)
    # No ack FIFO: an ack pipe nobody reads deadlocks perf.
    (directory / BUILDID_DIR).mkdir()
    prefix = [*perf_command(directory), "record", "-q", "-k", "mono", "--call-graph", call_graph, "--delay=-1", f"--control=fifo:{fifo}",
              "-e", event, *rate, "-o", str(directory / "perf.data"), "--"]
    perf_spec = replace(spec, prefix=prefix, env={**spec.env, "JITDUMPDIR": str(directory / "run")},
                        args=spec.args + ["--perf-ctl-fifo", str(fifo), "--perf-window",
                                          f"{window[0]}:{window[1]}", "--perf-jitdump"])
    result = run(perf_spec, directory / "run")
    if result.reached:
        inject = subprocess.run([*perf_command(directory), "inject", "--jit", "-i", str(directory / "perf.data"),
                                 "-o", str(directory / "perf.jit.data")], capture_output=True, text=True)
        if inject.returncode != 0:
            result.error = f"perf inject failed: {inject.stderr.strip()[-300:]}"
    stats = record_stats(directory / "perf.data") if (directory / "perf.data").is_file() else None
    (directory / "capture.json").write_text(json.dumps(
        {"event": event, "precise": is_precise(event), "rate": rate, "call_graph": call_graph,
         "window": list(window), "record": stats,
         "run": result.to_json()}, indent=1))
    return result
