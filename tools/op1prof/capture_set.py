"""Several perf captures of one configuration, and two such sets compared.

One capture's shares move between captures of the same configuration by
more than its sample counts suggest (2.5% between capture sets of one
configuration, 11% for a baseline, in the research line): an interval on a
block or bucket needs repeated captures, and a comparison of two
configurations needs a set of each."""
from __future__ import annotations

import math
import statistics
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

from . import stats
from .categories import Categories
from .marks import workload_diff
from . import report as perf_report

SECTIONS = (("buckets", "bucket", "seconds"), ("blocks", "block", "self_s"), ("blocks_incl", "block", "incl_s"),
            ("shared_code", "symbol", "seconds"), ("host_self", "symbol", "seconds"),
            ("host_inclusive", "symbol", "seconds"))
# The tables a set and a diff show, after the buckets and the blocks: the
# generated code no block owns (report.SHARED_PREFIX), then the host's.
SHARED_TABLE = ("generated code no block owns (self)", "shared_code", "symbol")
MIN_FOR_INTERVAL = 3
SET_SCHEMA = "op1prof.report-set.v1"
DIFF_SCHEMA = "op1prof.report-diff.v1"


def captures_under(path: Path) -> List[Path]:
    """A capture directory, or every capture directory directly under a
    directory (an op1prof perf --captures output, or ab --perf's side)."""
    if (path / "capture.json").is_file():
        return [path]
    found = sorted(p for p in path.iterdir() if (p / "capture.json").is_file()) if path.is_dir() else []
    if not found:
        raise ValueError(f"{path}: no capture (capture.json) here or directly below")
    return found


def _rows_of(report: dict, section: str, key: str, value: str) -> Dict[str, float]:
    rows = report["blocks"] if section == "blocks_incl" else report.get(section, [])
    return {row[key]: row[value] for row in rows if row.get(value) is not None}


def _interval(values: Sequence[float]) -> Optional[Tuple[float, float]]:
    n = len(values)
    if n < MIN_FOR_INTERVAL:
        return None
    half = stats.t975(n - 1) * statistics.stdev(values) / math.sqrt(n)
    mean = statistics.fmean(values)
    return mean - half, mean + half


def load(captures: List[Path], categories: Categories, labels: perf_report.Labels) -> List[dict]:
    """Analyze each capture; all must be one configuration in all but timing:
    the same window and event, and the same run (perf_report.check_same_run)."""
    seen = {}
    for capture in captures:
        real = capture.resolve()
        if real in seen:
            raise ValueError(f"{capture} is given twice (also as {seen[real]}): a capture counted twice is not a "
                             "repeat, and its interval would be too narrow")
        seen[real] = capture
    reports = [perf_report.analyze(c, categories, labels) for c in captures]
    for capture, report in zip(captures, reports):
        if not report["samples"].get("execution", {}).get("count"):
            # Every row would read as zero: an estimate of nothing, not of no time.
            raise ValueError(f"{capture}: no execution samples in the window (too short for the sampling period): "
                             "its time cannot be estimated; capture a longer window or sample more often")
    first = reports[0]
    for capture, report in zip(captures[1:], reports[1:]):
        for field in ("window", "event", "rate", "call_graph"):
            if report[field] != first[field]:
                raise ValueError(f"{capture}: {field} {report[field]} differs from {captures[0]}'s {first[field]}")
        perf_report.check_same_run(captures[0], capture / "run")
    return reports


def aggregate(captures: List[Path], reports: List[dict]) -> dict:
    """Per row of each table, the seconds of every capture (0 where a capture
    has no such row), their mean and, from three captures on, a 95% t
    interval."""
    n = len(reports)
    out = {"schema": SET_SCHEMA, "captures": [str(c) for c in captures], "window": reports[0]["window"],
           "event": reports[0]["event"], "rate": reports[0]["rate"], "call_graph": reports[0]["call_graph"],
           "execution_cpu_s": [r["execution_cpu_s"] for r in reports],
           "inclusive_valid": all(r["inclusive_valid"] for r in reports),
           "record": [r.get("record") for r in reports]}
    for section, key, value in SECTIONS:
        per = [_rows_of(r, section, key, value) for r in reports]
        names = set().union(*per)
        rows = []
        for name in names:
            values = [p.get(name, 0.0) for p in per]
            interval = _interval(values)
            rows.append({key: name, "values": values, "mean": statistics.fmean(values),
                         "ci95": list(interval) if interval else None})
        rows.sort(key=lambda r: -r["mean"])
        out[section] = rows
    out["n"] = n
    return out


def _cell(mean: float, interval: Optional[List[float]]) -> str:
    if interval is None:
        return f"{mean:9.3f} {'':>15}"
    return f"{mean:9.3f} {interval[0]:7.3f}..{interval[1]:<7.3f}"


def format_set(data: dict, limit: int) -> str:
    n = data["n"]
    execution = statistics.fmean(data["execution_cpu_s"])
    lines = [f"perf {data['event']} {' '.join(data['rate'])} --call-graph {data['call_graph']}, window "
             f"{data['window'][0]} -> {data['window'][1]}: {n} captures, execution CPU "
             + ", ".join(f"{v:.3f}" for v in data["execution_cpu_s"]) + " s"]
    lines.append("seconds: mean over the captures and its 95% t interval" if n >= MIN_FOR_INTERVAL else
                 f"seconds: mean of {n} captures; an interval needs {MIN_FOR_INTERVAL} or more")
    lines += _record_warnings(data["record"])
    tables = [("host-path bucket", "buckets", "bucket"), ("guest block (self)", "blocks", "block")]
    if data["inclusive_valid"]:
        tables += [("guest block (inclusive)", "blocks_incl", "block"), ("host symbol (inclusive)", "host_inclusive", "symbol")]
    if data["shared_code"]:
        tables.append(SHARED_TABLE)
    tables.append(("host symbol (self)", "host_self", "symbol"))
    for title, section, key in tables:
        lines += ["", f"{title:56s} {'seconds':>9} {'95% interval':>15} {'share':>6}"]
        for row in data[section][:limit]:
            lines.append(f"{row[key][:56]:56s} {_cell(row['mean'], row['ci95'])} "
                         f"{row['mean'] / execution * 100 if execution else 0:5.1f}%")
    return "\n".join(lines)


def _record_warnings(records: List[Optional[dict]]) -> List[str]:
    lines = []
    for index, record in enumerate(records):
        if record and record.get("warning"):
            lines.append(f"capture {index}: {record['warning']}")
    return lines


def _welch(a: Sequence[float], b: Sequence[float]) -> Optional[Tuple[float, float]]:
    """95% interval of mean(b) - mean(a) for two independent sets."""
    if len(a) < MIN_FOR_INTERVAL or len(b) < MIN_FOR_INTERVAL:
        return None
    va, vb = statistics.variance(a) / len(a), statistics.variance(b) / len(b)
    delta = statistics.fmean(b) - statistics.fmean(a)
    if va + vb == 0:
        return delta, delta
    df = (va + vb) ** 2 / (va ** 2 / (len(a) - 1) + vb ** 2 / (len(b) - 1))
    half = stats.t975(int(df)) * math.sqrt(va + vb)
    return delta - half, delta + half


def diff_sets(a_captures: List[Path], a: List[dict], b_captures: List[Path], b: List[dict]) -> dict:
    """Per row, the mean seconds of each set, their difference and, with three
    or more captures a side, a 95% Welch interval of it."""
    overlap = {c.resolve() for c in a_captures} & {c.resolve() for c in b_captures}
    if overlap:
        raise ValueError(f"captures on both sides: {sorted(map(str, overlap))[:3]}")
    for field in ("window", "event", "rate", "call_graph"):
        if a[0][field] != b[0][field]:
            raise ValueError(f"the sets differ in {field}: {a[0][field]} vs {b[0][field]}; capture both alike")
    sa, sb = aggregate(a_captures, a), aggregate(b_captures, b)
    marks_a = perf_report._marks(perf_report._run_entry(a_captures[0] / "run")["marks"])
    marks_b = perf_report._marks(perf_report._run_entry(b_captures[0] / "run")["marks"])
    out = {"schema": DIFF_SCHEMA, "a": sa["captures"], "b": sb["captures"], "window": sa["window"],
           "event": sa["event"], "rate": sa["rate"], "call_graph": sa["call_graph"],
           "execution_cpu_s": {"a": sa["execution_cpu_s"], "b": sb["execution_cpu_s"]},
           "workload_diff": workload_diff(marks_a, marks_b),
           "inclusive_valid": sa["inclusive_valid"] and sb["inclusive_valid"],
           "record": {"a": sa["record"], "b": sb["record"]}}
    for section, key, _ in SECTIONS:
        rows_a = {r[key]: r for r in sa[section]}
        rows_b = {r[key]: r for r in sb[section]}
        rows = []
        for name in set(rows_a) | set(rows_b):
            va = rows_a[name]["values"] if name in rows_a else [0.0] * sa["n"]
            vb = rows_b[name]["values"] if name in rows_b else [0.0] * sb["n"]
            interval = _welch(va, vb)
            rows.append({key: name, "a": statistics.fmean(va), "b": statistics.fmean(vb),
                         "delta": statistics.fmean(vb) - statistics.fmean(va), "ci95": list(interval) if interval else None})
        rows.sort(key=lambda r: -abs(r["delta"]))
        out[section] = rows
    out["n"] = {"a": sa["n"], "b": sb["n"]}
    return out


def format_diff(data: dict, limit: int) -> str:
    n = data["n"]
    lines = [f"perf diff {data['event']} {' '.join(data['rate'])}, window {data['window'][0]} -> "
             f"{data['window'][1]}: A {n['a']} captures, B {n['b']}",
             "execution CPU A " + ", ".join(f"{v:.3f}" for v in data["execution_cpu_s"]["a"]) + " s; B "
             + ", ".join(f"{v:.3f}" for v in data["execution_cpu_s"]["b"]) + " s"]
    if data["workload_diff"]:
        lines.append("the sets did different guest work: " + "; ".join(data["workload_diff"][:3]))
    if min(n.values()) < MIN_FOR_INTERVAL:
        lines.append(f"no intervals: they need {MIN_FOR_INTERVAL} captures a side. One configuration's shares "
                     "moved 2.5% between capture sets and a baseline's 11%: a single capture's difference is "
                     "not evidence.")
    else:
        lines.append("delta: B - A in seconds, with a 95% Welch interval; an interval that excludes 0 is a "
                     "difference in where the time went, not in total time (that is ab's)")
        rows = [r for section, _, _ in SECTIONS for r in data[section] if r["ci95"]]
        beyond = sum(1 for r in rows if r["ci95"][0] > 0 or r["ci95"][1] < 0)
        # Every row is a test of its own: across hundreds, some exclude 0 by
        # chance (an A/A of three captures a side: 10 of 689).
        lines.append(f"{beyond} of {len(rows)} intervals exclude 0; with this many rows some do by chance, so "
                     "trust a difference that is large, or that a second set repeats")
    for side in ("a", "b"):
        lines += [f"{side.upper()} " + line for line in _record_warnings(data["record"][side])]
    tables = [("host-path bucket", "buckets", "bucket"), ("guest block (self)", "blocks", "block")]
    if data["inclusive_valid"]:
        tables += [("guest block (inclusive)", "blocks_incl", "block"), ("host symbol (inclusive)", "host_inclusive", "symbol")]
    if data["shared_code"]:
        tables.append(SHARED_TABLE)
    tables.append(("host symbol (self)", "host_self", "symbol"))
    for title, section, key in tables:
        lines += ["", f"{title:50s} {'A s':>9} {'B s':>9} {'delta':>9} {'95% interval':>17}"]
        for row in data[section][:limit]:
            ci = f"{row['ci95'][0]:+8.3f}..{row['ci95'][1]:<+8.3f}" if row["ci95"] else ""
            lines.append(f"{row[key][:50]:50s} {row['a']:9.3f} {row['b']:9.3f} {row['delta']:+9.3f} {ci:>17}")
    return "\n".join(lines)
