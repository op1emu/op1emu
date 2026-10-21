"""Arguments and checks the op1prof commands share."""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from .. import machine
from .. import marks as marks_file
from ..runner import REPO, RunSpec, instrument_args, reserved_args

# Other threads' CPU (frontend, USB/IP loop) worth a warning, relative to execution CPU.
OTHER_THREADS_WARN = 0.05
# A missing mark, file or field in a capture, or perf script failing.
REPORT_ERRORS = (ValueError, KeyError, OSError, RuntimeError)
DEFAULT_MARKS = REPO / "profiling" / "marks" / "op1-stock-nand.json"

def _common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--nand", required=True, type=Path, help="NAND image (opened copy-on-write)")
    parser.add_argument("--otp", required=True, type=Path, help="OTP image, copied into each run")
    parser.add_argument("--marks", type=Path, default=DEFAULT_MARKS, help="phase marks file")
    parser.add_argument("--until", default="main-frame", help="mark that ends each run")
    parser.add_argument("--phase", default=None, help="BEGIN:END marks for time metrics (default start:UNTIL)")
    parser.add_argument("--arg", action="append", default=[], help="extra emulator argument (repeatable)")
    parser.add_argument("--cpu", type=int, default=None, help="pin the emulator to this CPU")
    parser.add_argument("--timeout", type=float, default=3600.0, help="seconds per run")
    parser.add_argument("--progress-timeout", type=float, default=900.0,
                        help="seconds without a new mark before a run counts as hung")
    parser.add_argument("--min-mem-gb", type=float, default=10.0, help="refuse to start below this MemAvailable")
    parser.add_argument("--force", action="store_true", help="start despite preflight problems (recorded)")
    parser.add_argument("--out", type=Path, default=None, help="results directory (default op1prof-<time>)")
    parser.add_argument("--script", type=Path, default=None, metavar="FILE",
                        help="headless input script for the runs (default: fixed sensors, then wait); see "
                             "tools/op1prof/scripts/")
    parser.add_argument("--end-cycles", type=_positive, default=None, metavar="N",
                        help="fire the mark 'end' at the first block boundary at or past N guest cycles, for phases "
                             "after the boot (--until end)")
    parser.add_argument("--trace-events", type=_positive, default=None, metavar="N",
                        help="trace recorder capacity in events, for runs longer than a stock boot (default 262144)")


def _checksum(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--checksum", type=_positive, metavar="N",
                        help="record guest-state checksums every N blocks, for op1prof diverge (diagnostic)")
    parser.add_argument("--checksum-from", type=int, default=0, metavar="RUNS",
                        help="first block to record (narrow down a divergence)")


def _hex(text: str) -> int:
    value = int(text, 16)
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError(f"not a 32-bit address: {text}")
    return value


def _dump_pc(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--dump-pc", type=_hex, action="append", metavar="PC",
                        help="write the lifted and optimized IR of the block at PC into ir/ (repeatable)")


def _counters(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--counters", action="store_true",
                        help="count the CPU thread's user-space instructions and cycles (perf_event_paranoid <= 2; "
                             "pin --cpu to a performance core)")


def _out(args, kind: str) -> Path:
    out = args.out or Path(f"op1prof-{kind}-{time.strftime('%Y%m%d-%H%M%S')}")
    out.mkdir(parents=True, exist_ok=False)
    return out.resolve()


def _positive(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError(f"must be at least 1, got {value}")
    return value


def _preflight(args, compares: bool = False) -> bool:
    problems = machine.preflight(args.cpu, args.min_mem_gb, require_pin=compares)
    for problem in problems:
        print(f"preflight: {problem}", file=sys.stderr)
    if problems and not args.force:
        print("preflight failed; fix the machine or pass --force", file=sys.stderr)
        return False
    return True


def _check_args(extra) -> None:
    reserved = reserved_args(list(extra))
    if reserved:
        raise SystemExit(f"extra emulator arguments may not set what every run fixes: {reserved}")


def _check_timing_args(extra) -> None:
    """ab compares time: diagnostic instrumentation, on either side, would be
    part of what it measures."""
    _check_args(extra)
    instrumented = instrument_args(list(extra))
    if instrumented:
        raise SystemExit(f"ab measures time and refuses diagnostic instrumentation {instrumented}: use run --census, "
                         "run --probes, run --checksum or perf for those")


def _spec(args, binary: Path) -> RunSpec:
    for path in (args.nand, args.otp, args.marks):
        if not path.is_file():
            raise SystemExit(f"not a file: {path}")
    _check_args(args.arg)
    script = getattr(args, "script", None)
    if script is not None and not script.is_file():
        raise SystemExit(f"not a file: {script}")
    extra = {"script": script.read_text()} if script is not None else {}
    return RunSpec(binary=binary, **extra, end_cycles=getattr(args, "end_cycles", None), nand=args.nand.resolve(), otp=args.otp.resolve(),
                   marks=args.marks.resolve(), until=args.until, args=list(args.arg),
                   timeout_s=args.timeout, progress_timeout_s=args.progress_timeout, cpu=args.cpu,
                   counters=getattr(args, "counters", False),
                   checksum_every=getattr(args, "checksum", None), checksum_from=getattr(args, "checksum_from", 0),
                   probes=getattr(args, "probes", False), dump_pcs=getattr(args, "dump_pc", None) or [],
                   trace_events=getattr(args, "trace_events", None))


def _phase(args) -> tuple:
    """The phase of the time metrics, checked against the marks file before
    any run: a bad name found afterwards would cost the whole run."""
    try:
        phase = marks_file.parse_phase(args.phase) if args.phase else ("start", args.until)
        problem = marks_file.check_phase(args.marks, phase, args.until)
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(f"--phase/--until: {error}")
    if problem:
        raise SystemExit(f"--phase/--until: {problem}")
    return phase
