"""Attribute a perf capture: guest blocks, host symbols, host-path buckets,
and, joined with a census run, cost per executed block."""
from __future__ import annotations

import bisect
import itertools
import json
import re
import subprocess
import tempfile
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Optional, Sequence, Tuple

from . import census, compile_samples, exits, perf, trace
from .perf import is_software_event
from .categories import Categories
from .marks import Mark, workload_diff
from .runner import RUN_FILE

SCHEMA = "op1prof.report.v1"
# Below this share of execution weight with a call chain that reaches the
# CPU loop, inclusive (caller) numbers would silently undercount.
MIN_COMPLETE_PCT = 95.0
BLOCK_PREFIX = "bb_0x"
# Generated code that is no block's: what bcore's copy-and-patch blocks
# share in each arena (bcore_cp_exit, their chain exit, and
# bcore_cp_jump_<helper>, their calls' jumps to the helpers).
SHARED_PREFIX = "bcore_cp_"

# With the event field (perf script -F ...,event,...): "tid time: period event: ".
HEADER = re.compile(r"^\s*(?P<tid>\d+)\s+(?P<time>\d+\.\d+):\s+(?P<period>\d+)(?:\s+(?P<event>\S+):)?\s*$")
FRAME = re.compile(r"^\s+(?P<ip>[0-9a-f]+)\s+(?P<sym>.*)\s+\((?P<dso>[^()]*)\)\s*$")
# `perf script -G`: the header and the sampled IP's frame on one line.
LEAF = re.compile(r"^\s*(?P<tid>\d+)\s+(?P<time>\d+\.\d+):\s+(?P<period>\d+)(?:\s+(?P<event>\S+):)?"
                  r"\s+(?P<ip>[0-9a-f]+)\s+(?P<sym>.*)\s+\((?P<dso>[^()]*)\)\s*$")
# The fields perf script prints (_perf_script).
SCRIPT_FIELDS = "tid,time,event,period,ip,sym,dso"


@dataclass(frozen=True)
class Sample:
    tid: int
    time_ns: int
    period: int
    frames: Tuple[Tuple[str, str], ...]   # (symbol, dso basename), leaf first
    event: str = ""                       # perf's name for it; "" where the script had no event field

    @property
    def symbols(self) -> Tuple[str, ...]:
        return tuple(symbol for symbol, _ in self.frames)


def parse_samples(lines: Iterable[str]) -> Iterator[Sample]:
    """Samples from `perf script -F tid,time,period,ip,sym,dso --ns` with
    call chains: a header line, one line per frame, a blank line."""
    header, frames = None, []
    for line in lines:
        match = HEADER.match(line)
        if match:
            if header:
                yield _sample(header, frames)
            header, frames = match, []
            continue
        match = FRAME.match(line)
        if header and match:
            frames.append((match.group("sym"), match.group("dso").rsplit("/", 1)[-1]))
        elif header and not line.strip():
            yield _sample(header, frames)
            header, frames = None, []
    if header:
        yield _sample(header, frames)


def parse_leaves(lines: Iterable[str]) -> Iterator[Sample]:
    """Samples from `perf script -G -F tid,time,period,ip,sym,dso --ns`:
    each with the one frame of its sampled IP."""
    for line in lines:
        if not line.strip():
            continue
        match = LEAF.match(line)
        if not match:
            raise ValueError(f"cannot parse a perf script line without call chains: {line.strip()[:120]!r}")
        yield _sample(match, [(match.group("sym"), match.group("dso").rsplit("/", 1)[-1])])


def with_leaves(samples: Iterable[Sample], leaves: Iterable[Sample]) -> Iterator[Sample]:
    """Each sample with its leaf frame replaced by the function of its
    sampled IP. An LBR call stack starts at the target of the last call,
    which is where the IP is unless that function jumped elsewhere since:
    generated code runs the blocks it chains by musttail jumps, so every
    block after the first of a run had its time charged to the first.
    `leaves` are the same samples in the same order (`parse_leaves`)."""
    # In step, one sample of each at a time: a boot's capture holds millions
    # (reading them all first once took more memory than the machine had).
    done = object()
    samples, leaves = iter(samples), iter(leaves)
    for index in itertools.count():
        sample, leaf = next(samples, done), next(leaves, done)
        if sample is done or leaf is done:
            if sample is not leaf:
                more, fewer = ("without", "with") if sample is done else ("with", "without")
                raise ValueError(f"perf script gave more samples {more} call chains than {fewer} them "
                                 f"(from sample {index})")
            return
        if (sample.tid, sample.time_ns, sample.period, sample.event) != (leaf.tid, leaf.time_ns, leaf.period,
                                                                          leaf.event):
            raise ValueError(f"perf script sample {index} differs with and without call chains")
        yield Sample(tid=sample.tid, time_ns=sample.time_ns, period=sample.period,
                     frames=leaf.frames + sample.frames[1:], event=sample.event)


def _sample(header: re.Match, frames: List[Tuple[str, str]]) -> Sample:
    seconds, _, fraction = header.group("time").partition(".")
    return Sample(tid=int(header.group("tid")),
                  time_ns=int(seconds) * 1_000_000_000 + int(fraction.ljust(9, "0")[:9]),
                  period=int(header.group("period")), frames=tuple(frames), event=header.group("event") or "")


def block_pc(symbol: str) -> Optional[int]:
    """The guest PC of a block function, or of its cold part
    (bb_0x<pc>.cold, where bcore's backend split the block), whatever
    variant: .careful (compiled again after a loop-bottom guard miss), .t1
    (promoted from tier 0)."""
    if not symbol.startswith(BLOCK_PREFIX):
        return None
    try:
        return int(symbol[len(BLOCK_PREFIX) - 2:].removesuffix(".cold").removesuffix(".t1").removesuffix(".careful"), 16)
    except ValueError:
        return None


def shared_code(symbol: str) -> bool:
    """Generated code that belongs to no block (SHARED_PREFIX)."""
    return symbol.startswith(SHARED_PREFIX)


@dataclass(frozen=True)
class PcRange:
    name: str
    lo: int
    hi: int

    @staticmethod
    def parse(text: str) -> "PcRange":
        name, _, span = text.partition("=")
        lo, _, hi = span.partition(":")
        if not name or not hi:
            raise ValueError(f"--pc-range must be NAME=LO:HI, got {text!r}")
        span = PcRange(name, int(lo, 16), int(hi, 16))
        if span.lo > span.hi:
            raise ValueError(f"--pc-range {text!r} has LO above HI")
        return span


@dataclass
class Labels:
    """Names for guest blocks: a PC range merges its blocks into one row."""
    symbols: census.Symbols = field(default_factory=lambda: census.Symbols(None))
    ranges: Sequence[PcRange] = ()

    def __call__(self, pc: int) -> str:
        for span in self.ranges:
            if span.lo <= pc <= span.hi:
                return span.name
        name = self.symbols.name(pc)
        return f"{BLOCK_PREFIX}{pc:08x}" + (f" {name}" if name else "")


@dataclass
class Attribution:
    counts: Counter = field(default_factory=Counter)     # samples by fate
    weights: Counter = field(default_factory=Counter)    # period by fate
    complete: int = 0                                    # execution weight with a whole chain
    self_blocks: Counter = field(default_factory=Counter)
    incl_blocks: Counter = field(default_factory=Counter)
    shared_code: Counter = field(default_factory=Counter)  # leaf in generated code no block owns
    self_symbols: Counter = field(default_factory=Counter)
    incl_symbols: Counter = field(default_factory=Counter)
    buckets: Counter = field(default_factory=Counter)
    callers: Counter = field(default_factory=Counter)
    # The JIT's samples: the CPU thread's inside translations, and bcore's
    # worker thread's in the window (compile_samples), by stage and by pass.
    compile_stages: Counter = field(default_factory=Counter)
    compile_passes: Counter = field(default_factory=Counter)
    worker_stages: Counter = field(default_factory=Counter)
    worker_passes: Counter = field(default_factory=Counter)
    event: str = ""
    # A capture of several events: this is the first's (the one the time
    # is divided by); the others, each attributed alike.
    others: Dict[str, "Attribution"] = field(default_factory=dict)

    @property
    def execution(self) -> int:
        return self.weights["execution"]

    @property
    def worker(self) -> int:
        return self.weights["worker"]


def _merged(spans: Iterable[Tuple[int, int]]) -> List[Tuple[int, int]]:
    """Disjoint, sorted intervals, so one bisect finds the span covering a time."""
    out: List[Tuple[int, int]] = []
    for begin, end in sorted(spans):
        if out and begin <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], end))
        else:
            out.append((begin, end))
    return out


# DSO names that differ between runs of the same code: op1prof's binary
# snapshot (op1emu-<content hash>) and perf inject's jitted-<pid>-<n>.so.
SNAPSHOT_DSO = re.compile(r"^op1emu-[0-9a-f]{12}$")
JITTED_DSO = re.compile(r"^jitted-\d+-\d+\.so$")


def dso_label(dso: str) -> str:
    """A DSO as a row label: the same name in every capture of every binary,
    so one function compares with itself."""
    if SNAPSHOT_DSO.match(dso):
        return "op1emu"
    if JITTED_DSO.match(dso):
        return "jitted"
    return dso


def attribute(samples: Iterable[Sample], trace_data: dict, window: Tuple[str, str], categories: Categories,
              labels: Labels, callers_of: Optional[str] = None, events: Sequence[str] = ()) -> Attribution:
    """Weight every CPU-thread sample inside the window and outside
    translation by its period. Samples of several events are attributed
    each by itself: the result is the first of `events` that has samples
    (perf's own order, `perf evlist`), else the first seen, with the others
    in its `others`."""
    found = trace.marks(trace_data)
    lo, hi = (int(found[name]["ts"] * 1000) for name in window)
    spans = _merged((int(e["ts"] * 1000), int((e["ts"] + e["dur"]) * 1000))
                    for e in trace.jit_spans(trace_data))
    starts = [s for s, _ in spans]
    tid = trace_data.get("cpu_thread_tid")
    if tid is None:
        # Every thread's samples would count as the CPU thread's.
        raise ValueError("the trace does not name the CPU thread (cpu_thread_tid): record it with a newer binary")
    by_event: Dict[str, Attribution] = {}
    for sample in samples:
        out = by_event.get(sample.event)
        if out is None:
            out = by_event[sample.event] = Attribution(event=sample.event)
        inside = lo <= sample.time_ns < hi
        if sample.tid != tid:
            # bcore's worker thread compiling promotions: JIT time of its own.
            fate = "worker" if inside and compile_samples.is_worker(sample.symbols) else "other_threads"
        elif not inside:
            fate = "outside_window"
        else:
            index = bisect.bisect_right(starts, sample.time_ns) - 1
            fate = "compile" if index >= 0 and sample.time_ns < spans[index][1] else "execution"
        out.counts[fate] += 1
        out.weights[fate] += sample.period
        if fate == "execution":
            _execution(out, sample, categories, labels, callers_of)
        elif fate in ("compile", "worker"):
            stage, pass_ = compile_samples.classify(sample.symbols)
            (out.compile_stages if fate == "compile" else out.worker_stages)[stage] += sample.period
            (out.compile_passes if fate == "compile" else out.worker_passes)[pass_] += sample.period
    if not by_event:
        return Attribution()
    order = [event for event in events if event in by_event] + [e for e in by_event if e not in events]
    first = by_event[order[0]]
    first.others = {event: by_event[event] for event in order[1:]}
    return first


def _execution(out: Attribution, sample: Sample, categories: Categories, labels: Labels,
               callers_of: Optional[str]) -> None:
    weight, symbols = sample.period, sample.symbols
    out.buckets[categories.bucket(symbols)] += weight
    leaf_pc = block_pc(symbols[0]) if symbols else None
    if leaf_pc is not None:
        out.self_blocks[labels(leaf_pc)] += weight
    elif symbols and shared_code(symbols[0]):
        out.shared_code[symbols[0]] += weight
    elif sample.frames:
        out.self_symbols[f"{sample.frames[0][0]} ({dso_label(sample.frames[0][1])})"] += weight
    # A block's inclusive time covers what it calls (memory callbacks,
    # devices reached through them); one block per stack in this engine.
    block = next((pc for pc in map(block_pc, symbols) if pc is not None), None)
    if block is not None:
        out.incl_blocks[labels(block)] += weight
    if not categories.complete(symbols):
        return
    out.complete += weight
    for symbol in set(s for s in symbols if block_pc(s) is None and not shared_code(s)):
        out.incl_symbols[symbol] += weight
    if callers_of:
        for depth, symbol in enumerate(symbols):
            if callers_of in symbol:
                out.callers[symbols[depth + 1] if depth + 1 < len(symbols) else "(root)"] += weight
                break


def _rows(counter: Counter, execution: int, seconds_per_weight: float, key: str) -> List[dict]:
    return [{key: name, "seconds": weight * seconds_per_weight, "pct": weight / execution * 100}
            for name, weight in counter.most_common()]


def _marks(entries: List[dict]) -> List[Mark]:
    keys = ("name", "pc", "wall_ms", "cpu_ms")
    return [Mark(name=e["name"], pc=e["pc"], wall_ms=e["wall_ms"], cpu_ms=e["cpu_ms"],
                 counters={k: v for k, v in e.items() if k not in keys}) for e in entries]


def _run_entry(directory: Path) -> dict:
    path = directory / RUN_FILE
    if not path.is_file():
        raise ValueError(f"{directory}: no {RUN_FILE} (recorded by an older op1prof); record the run again")
    return json.loads(path.read_text())


def check_same_run(capture: Path, census_run: Path) -> None:
    """A census run may price a capture's samples only if it is the same run
    in all but timing: the same binary and inputs, the same guest work, and
    the same blocks (a count per block is what the samples are divided by,
    and the workload gate deliberately ignores block counts)."""
    if not (census_run / "log.txt").is_file():
        raise ValueError(f"{census_run}: no log.txt; pass the run/ directory of `op1prof run --census`")
    capture_run, census = _run_entry(capture / "run"), _run_entry(census_run)
    if not capture_run.get("binary_sha256") or capture_run["binary_sha256"] != census.get("binary_sha256"):
        raise ValueError("the census run used another binary than the capture "
                         f"({census.get('binary_sha256')} vs {capture_run.get('binary_sha256')})")
    for key, value in capture_run.get("inputs", {}).items():
        if census.get("inputs", {}).get(key) != value:
            raise ValueError(f"the census run read another input than the capture: {key}")
    # Settings the guest sees (an RTC epoch, the input script): equal counts
    # do not prove equal state.
    for key in ("guest_args", "script_sha256"):
        if key not in capture_run or key not in census:
            raise ValueError(f"a run.json without {key} (recorded by an older op1prof); record the run again")
        if capture_run[key] != census[key]:
            raise ValueError(f"the census run had another {key.replace('_sha256', '')} than the capture "
                             f"({census[key]} vs {capture_run[key]})")
    capture_marks, census_marks = _marks(capture_run["marks"]), _marks(census["marks"])
    diff = workload_diff(capture_marks, census_marks)
    if diff:
        raise ValueError("the census run did different guest work than the capture: " + "; ".join(diff[:5]))
    census_by = {m.name: m for m in census_marks}
    for mark in capture_marks:
        other = census_by.get(mark.name)
        if other and other.counters.get("runs") != mark.counters.get("runs"):
            raise ValueError(f"the census run executed another number of blocks than the capture at {mark.name} "
                             f"({other.counters.get('runs')} vs {mark.counters.get('runs')})")


def join_census(result: Attribution, census_run: Path, capture: Path, window: Tuple[str, str],
                labels: Labels, seconds_per_weight: float) -> dict:
    """Executed work from a census run of the same binary and inputs, priced
    by the capture's samples: seconds per block and per execution. The
    census changes timing, so it always comes from another run, and only
    the same run in all but timing may be joined (check_same_run)."""
    check_same_run(capture, census_run)
    counts: Dict[str, List[int]] = {}
    for pc, (runs, packets) in census.phase(census_run / "census", "pc", *window).items():
        row = counts.setdefault(labels(pc), [0, 0])
        row[0] += runs
        row[1] += packets
    # Memory accesses per block, from the exit census (newer census runs).
    accesses: Optional[Dict[str, int]] = None
    if Path(f"{census_run / 'census'}.exit.{window[1]}").is_file():
        accesses = {}
        for pc, (_, _, _, count) in exits.exit_phase(census_run / "census", *window).items():
            accesses[labels(pc)] = accesses.get(labels(pc), 0) + count
    total_runs = sum(runs for runs, _ in counts.values())
    in_blocks = sum(weight for name, weight in result.incl_blocks.items() if name in counts)
    # Sampled in a block the census never ran in this phase: a window edge,
    # or labels that differ between the runs. Neither a row nor host time.
    unmatched = sum(weight for name, weight in result.incl_blocks.items() if name not in counts)
    rows = []
    for name, (runs, packets) in counts.items():
        seconds = result.incl_blocks.get(name, 0) * seconds_per_weight
        row = {"block": name, "runs": runs, "packets": packets, "seconds": seconds,
               "ns_per_run": seconds * 1e9 / runs if runs else 0.0,
               "ns_per_packet": seconds * 1e9 / packets if packets else 0.0}
        if accesses is not None:
            count = accesses.get(name, 0)
            row["accesses_per_run"] = count / runs if runs else 0.0
            row["ns_per_access"] = seconds * 1e9 / count if count else None
        rows.append(row)
    rows.sort(key=lambda row: -row["seconds"])
    host = (result.execution - in_blocks - unmatched) * seconds_per_weight
    return {"window": list(window), "runs": total_runs, "blocks": rows,
            "sampled_not_in_census_s": unmatched * seconds_per_weight,
            "outside_blocks_s": host, "outside_blocks_ns_per_run": host * 1e9 / total_runs if total_runs else 0.0}


def analyze(capture: Path, categories: Categories, labels: Labels, callers_of: Optional[str] = None,
            census_run: Optional[Path] = None, samples: Optional[Iterable[Sample]] = None) -> dict:
    info = json.loads((capture / "capture.json").read_text())
    window = tuple(info["window"])
    trace_data = trace.load(str(capture / "run" / "trace.json"))
    events: List[str] = []
    if samples is None:
        events = perf.event_order(capture / "perf.jit.data")
        samples = parse_samples(_perf_script(capture / "perf.jit.data"))
        # A frame-pointer chain starts at the sampled IP already.
        if info.get("call_graph", "fp") == "lbr":
            samples = with_leaves(samples, parse_leaves(_perf_script(capture / "perf.jit.data", call_chains=False)))
    result = attribute(samples, trace_data, window, categories, labels, callers_of, events)
    phase = trace.phase(trace_data, *window)
    execution = result.execution
    per_weight = phase.noncompile_cpu_ms / 1e3 / execution if execution else 0.0
    complete_pct = result.complete / execution * 100 if execution else 0.0
    inclusive = complete_pct >= MIN_COMPLETE_PCT
    event = info["event"]
    data = {
        "schema": SCHEMA, "event": event, "precise": info["precise"], "rate": info["rate"],
        "call_graph": info.get("call_graph", "fp"),
        # Captures before the check have none in capture.json: read it now.
        "record": info.get("record") or (perf.record_stats(capture / "perf.data")
                                         if (capture / "perf.data").is_file() else None),
        "window": list(window),
        "samples": {fate: {"count": result.counts[fate], "weight": result.weights[fate]} for fate in result.counts},
        "execution_cpu_s": phase.noncompile_cpu_ms / 1e3, "compile_cpu_s": phase.compile_cpu_ms / 1e3,
        # For a time event the sampled period is CPU time: it should match
        # the trace's execution CPU, and a large gap means lost samples.
        "sampled_over_trace": (execution / 1e6 / phase.noncompile_cpu_ms
                               if is_software_event(event) and phase.noncompile_cpu_ms else None),
        "chain_complete_pct": complete_pct, "inclusive_valid": inclusive,
        # Blocks and the code they share: all of the generated code.
        "generated_code_pct": (sum(result.self_blocks.values()) + sum(result.shared_code.values())) / execution * 100
        if execution else 0.0,
        "shared_code": _rows(result.shared_code, execution, per_weight, "symbol") if execution else [],
        "buckets": _rows(result.buckets, execution, per_weight, "bucket") if execution else [],
        # Without whole chains a block's callees may be missing from its
        # inclusive time, so only self time is reported then.
        "blocks": [{"block": name, "self_s": result.self_blocks.get(name, 0) * per_weight,
                    "self_pct": result.self_blocks.get(name, 0) / execution * 100,
                    "incl_s": weight * per_weight if inclusive else None,
                    "incl_pct": weight / execution * 100 if inclusive else None}
                   for name, weight in (result.incl_blocks if inclusive else result.self_blocks).most_common()]
        if execution else [],
        "host_self": _rows(result.self_symbols, execution, per_weight, "symbol") if execution else [],
        "host_inclusive": _rows(result.incl_symbols, execution, per_weight, "symbol") if inclusive else [],
        "primary_event": result.event,
        "events": _other_events(result),
        **_jit_rows(result, phase),
    }
    if callers_of:
        data["callers"] = {"of": callers_of,
                           "rows": _rows(result.callers, execution, per_weight, "caller") if inclusive else []}
    if census_run:
        if not inclusive:
            raise ValueError(f"the census join needs whole call chains, but only {complete_pct:.1f}% reach the "
                             "CPU loop: capture with --call-graph lbr")
        data["census"] = join_census(result, census_run, capture, window, labels, per_weight)
    return data


def _jit_rows(result: Attribution, phase) -> dict:
    """The JIT's samples by stage and pass, in seconds of its CPU: the CPU
    thread's inside translations (compile_cpu_s), the worker's (its CPU in
    the window, bg_compile_cpu_ms; worker_exact False where that is an
    estimate, trace.bg_cpu_ms). CPU with no samples to split it leaves its
    tables empty: unknown, not zero (jit_unattributed)."""
    def rows(counter: Counter, total: int, seconds: float, key: str) -> List[dict]:
        if not total:
            return []
        return [{key: name, "seconds": weight * seconds / total, "pct": weight / total * 100}
                for name, weight in counter.most_common()]
    compile_s, worker_s = phase.compile_cpu_ms / 1e3, phase.bg_compile_cpu_ms / 1e3
    compiled, worked = result.weights["compile"], result.worker
    return {"compile_stages": rows(result.compile_stages, compiled, compile_s, "stage"),
            "compile_passes": rows(result.compile_passes, compiled, compile_s, "pass"),
            "worker_cpu_s": worker_s, "worker_exact": phase.bg_exact,
            "worker_stages": rows(result.worker_stages, worked, worker_s, "stage"),
            "worker_passes": rows(result.worker_passes, worked, worker_s, "pass")}


def _other_events(result: Attribution) -> List[dict]:
    """Each other event of the capture: its counts in execution, by guest
    block (self) and host-path bucket, beside the first event's."""
    return [{"event": event, "execution": other.execution,
             "blocks": dict(other.self_blocks.most_common()), "buckets": dict(other.buckets.most_common())}
            for event, other in result.others.items()]


def _perf_script(data: Path, call_chains: bool = True) -> Iterator[str]:
    """perf script's lines as it prints them: a boot's capture is millions
    of samples, more than the machine's memory as one list."""
    command = [*perf.perf_command(data.parent), "script", "-i", str(data), *([] if call_chains else ["-G"]),
               "-F", SCRIPT_FIELDS, "--ns"]
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors, text=True, errors="replace",
                                   bufsize=1 << 20)
        finished = False
        try:
            for line in process.stdout:
                yield line.rstrip("\n")
            finished = True
        finally:
            # A reader that stops early (an error in a sample) stops perf too.
            if not finished:
                process.kill()
            process.stdout.close()
            process.wait()
        if process.returncode != 0:
            errors.seek(0)
            raise RuntimeError(f"perf script failed: {errors.read().decode(errors='replace').strip()[-300:]}")


def _table(title: str, rows: List[dict], key: str, limit: int, width: int = 60) -> List[str]:
    lines = ["", f"{title:{width}s} {'seconds':>9} {'share':>6}"]
    for row in rows[:limit]:
        lines.append(f"{row[key][:width]:{width}s} {row['seconds']:9.3f} {row['pct']:5.1f}%")
    return lines


def short_event(event: str) -> str:
    """An event's name for a column: without the PMU of a hybrid CPU
    (cpu_core/...) and the user-only modifier."""
    name = re.sub(r"^cpu_(core|atom)/", "", event)
    return re.sub(r"(/u|:u)$", "", name).rstrip("/")


def _events_lines(data: dict, limit: int) -> List[str]:
    """A capture of several events: each block's and bucket's count of every
    event, in millions, and its ratio to the first event (instructions per
    cycle, say, where the first is cycles)."""
    others = data.get("events") or []
    if not others:
        return []
    first = data.get("primary_event") or data["event"]
    execution = data["samples"].get("execution", {}).get("weight", 0)
    names = [short_event(first)] + [short_event(o["event"]) for o in others]
    head = "".join(f" {name[:14]:>14}" for name in names) + "".join(f" {(name + '/' + names[0])[:20]:>20}"
                                                                     for name in names[1:])
    lines = ["", f"events: counts in execution, millions, and their ratios to {first}",
             f"{'guest block':40s}" + head]
    for row in data["blocks"][:limit]:
        weight = row["self_pct"] / 100 * execution
        counts = [weight] + [o["blocks"].get(row["block"], 0) for o in others]
        ratios = [c / weight if weight else 0.0 for c in counts[1:]]
        lines.append(f"{row['block'][:40]:40s}" + "".join(f" {c / 1e6:14.1f}" for c in counts)
                     + "".join(f" {r:20.3f}" for r in ratios))
    lines.append(f"{'host-path bucket':40s}" + head)
    for row in data["buckets"][:limit]:
        weight = row["pct"] / 100 * execution
        counts = [weight] + [o["buckets"].get(row["bucket"], 0) for o in others]
        ratios = [c / weight if weight else 0.0 for c in counts[1:]]
        lines.append(f"{row['bucket'][:40]:40s}" + "".join(f" {c / 1e6:14.1f}" for c in counts)
                     + "".join(f" {r:20.3f}" for r in ratios))
    totals = [execution] + [o["execution"] for o in others]
    lines.append(f"{'all execution':40s}" + "".join(f" {c / 1e6:14.1f}" for c in totals)
                 + "".join(f" {c / execution if execution else 0:20.3f}" for c in totals[1:]))
    return lines


# The JIT's tables, each with the CPU it splits: (stages, passes, CPU key).
JIT_SECTIONS = (("compile_stages", "compile_passes", "compile_cpu_s"), ("worker_stages", "worker_passes", "worker_cpu_s"))


def jit_unattributed(data: dict, section: str) -> bool:
    """Whether a JIT table is unknown: its CPU is there but no sample split it
    (a window too short for the sampling period, or the worker's stacks
    without --call-graph lbr, where CompileWorker:: is not found)."""
    cpu = next(cpu for stages, passes, cpu in JIT_SECTIONS if section in (stages, passes))
    return not data.get(section) and (data.get(cpu) or 0) > 0


def _jit_lines(data: dict, limit: int) -> List[str]:
    """The JIT's samples by stage and by pass: the CPU thread's inside
    translations, and bcore's worker thread's."""
    lines = []
    for who, (stages, passes, cpu) in zip(("CPU thread, inside translations", "JIT worker"), JIT_SECTIONS):
        cpu_s = data.get(cpu, 0)
        estimate = " (an estimate: no worker clock at the marks)" if cpu == "worker_cpu_s" and \
            not data.get("worker_exact", True) else ""
        if jit_unattributed(data, stages):
            lines += ["", f"JIT ({who}): {cpu_s:.3f} s of CPU{estimate}, but no samples to split it (a short window, "
                          "or the worker without --call-graph lbr): its stages are unknown"]
            continue
        if not data.get(stages):
            continue
        lines += ["", f"JIT ({who}): {cpu_s:.3f} s of CPU{estimate} split by sampled weight; a stage is the leaf-most "
                      "frame that names one, malloc counted with its caller's"]
        lines += _table(f"JIT stage ({who})", data[stages], "stage", limit)
        lines += _table(f"JIT pass ({who})", data[passes], "pass", limit)
    return lines


def format_report(data: dict, limit: int, sort: str = "seconds", min_runs: int = 1000) -> str:
    window = data["window"]
    lines = [f"perf {data['event']} {' '.join(data['rate'])} --call-graph {data['call_graph']}, "
             f"window {window[0]} -> {window[1]}",
             "samples: " + ", ".join(f"{fate} {v['count']}" for fate, v in data["samples"].items()),
             f"trace: execution CPU {data['execution_cpu_s']:.3f} s, compile CPU {data['compile_cpu_s']:.3f} s; "
             "seconds below are execution CPU split by sampled weight",
             f"generated code (self) {data['generated_code_pct']:.1f}% of execution",
             "Shares are where samples landed, not what removing that code would save."]
    record = data.get("record")
    if record and record.get("warning"):
        lines.append(f"warning: {record['warning']}")
    if data["sampled_over_trace"] is not None:
        lines.append(f"sampled CPU / trace execution CPU = {data['sampled_over_trace']:.3f}")
    if not data["precise"]:
        lines.append(f"{data['event']} is not a precise event: use these totals by block and symbol, "
                     "never to rank single instructions (use cycles:upp for that).")
    lines.append(f"call chains reaching the CPU loop: {data['chain_complete_pct']:.1f}% of execution weight")
    if not data["inclusive_valid"]:
        lines.append(f"  below {MIN_COMPLETE_PCT:g}%: inclusive and caller tables are withheld (code without "
                     "frame pointers breaks the chain); buckets use the frames nearest the leaf"
                     + ("; capture with --call-graph lbr" if data["call_graph"] == "fp" else ""))
    lines += _table("host-path bucket", data["buckets"], "bucket", limit)
    lines += ["", f"{'guest block':40s} {'self s':>9} {'self':>6} {'incl s':>9} {'incl':>6}"]
    for row in data["blocks"][:limit]:
        incl = f"{row['incl_s']:9.3f} {row['incl_pct']:5.1f}%" if row["incl_s"] is not None else f"{'-':>9} {'-':>6}"
        lines.append(f"{row['block'][:40]:40s} {row['self_s']:9.3f} {row['self_pct']:5.1f}% {incl}")
    if data.get("shared_code"):
        lines += _table("generated code no block owns (self)", data["shared_code"], "symbol", limit)
    lines += _table("host symbol (self)", data["host_self"], "symbol", limit)
    if data["host_inclusive"]:
        lines += _table("host symbol (inclusive)", data["host_inclusive"], "symbol", limit)
    if "callers" in data:
        lines += _table(f"callers of {data['callers']['of']}", data["callers"]["rows"], "caller", limit)
    lines += _events_lines(data, limit)
    lines += _jit_lines(data, limit)
    if "census" in data:
        lines += _census_lines(data["census"], limit, sort, min_runs)
    return "\n".join(lines)


def _census_lines(joined: dict, limit: int, sort: str, min_runs: int = 1000) -> List[str]:
    key = {"seconds": "seconds", "ns-per-run": "ns_per_run", "ns-per-packet": "ns_per_packet"}[sort]
    rows = joined["blocks"]
    left_out = 0
    if key != "seconds":
        # A block that ran a few times has a few samples: its cost per run is
        # chance, and dividing by a small count puts it on top.
        kept = [row for row in rows if row["runs"] >= min_runs]
        left_out, rows = len(rows) - len(kept), kept
    rows = sorted(rows, key=lambda row: -row[key])
    lines = ["", f"census join over {joined['runs']} runs: sampled estimates, not exact costs; "
                 "their error comes from repeated captures, not from sample counts",
             f"outside guest blocks (dispatch and the blocks' shared code, devices, event queue, CPU loop): "
             f"{joined['outside_blocks_s']:.3f} s = {joined['outside_blocks_ns_per_run']:.1f} ns per run; "
             f"sampled in blocks the census did not run: {joined['sampled_not_in_census_s']:.3f} s",
             *([f"{left_out} blocks that ran fewer than {min_runs} times are left out of this order (--min-runs)"]
               if left_out else []),
             f"{'guest block':40s} {'runs':>12} {'packets':>13} {'seconds':>9} {'ns/run':>8} {'ns/pkt':>7}"
             + (f" {'acc/run':>8} {'ns/acc':>7}" if rows and "accesses_per_run" in rows[0] else "")]
    for row in rows[:limit]:
        line = (f"{row['block'][:40]:40s} {row['runs']:>12} {row['packets']:>13} {row['seconds']:9.3f} "
                f"{row['ns_per_run']:8.1f} {row['ns_per_packet']:7.2f}")
        if "accesses_per_run" in row:
            per = row["ns_per_access"]
            line += f" {row['accesses_per_run']:8.1f} {per if per is not None else 0:7.2f}"
        lines.append(line)
    return lines
