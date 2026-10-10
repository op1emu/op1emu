"""op1prof command line: run, verify, ab."""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from . import ab, machine, trace, verify
from .runner import REPO, RunSpec, run, sha256_file, snapshot_binary

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


def _out(args, kind: str) -> Path:
    out = args.out or Path(f"op1prof-{kind}-{time.strftime('%Y%m%d-%H%M%S')}")
    out.mkdir(parents=True, exist_ok=False)
    return out.resolve()


def _positive(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError(f"must be at least 1, got {value}")
    return value


def _preflight(args) -> bool:
    problems = machine.preflight(args.cpu, args.min_mem_gb)
    for problem in problems:
        print(f"preflight: {problem}", file=sys.stderr)
    if problems and not args.force:
        print("preflight failed; fix the machine or pass --force", file=sys.stderr)
        return False
    return True


def _spec(args, binary: Path) -> RunSpec:
    for path in (args.nand, args.otp, args.marks):
        if not path.is_file():
            raise SystemExit(f"not a file: {path}")
    return RunSpec(binary=binary, nand=args.nand.resolve(), otp=args.otp.resolve(),
                   marks=args.marks.resolve(), until=args.until, args=list(args.arg),
                   timeout_s=args.timeout, progress_timeout_s=args.progress_timeout, cpu=args.cpu)


def _phase(args) -> tuple:
    if args.phase:
        begin, _, end = args.phase.partition(":")
        if not end:
            raise SystemExit("--phase must be BEGIN:END")
        return begin, end
    return "start", args.until


def cmd_run(args) -> int:
    if not _preflight(args):
        return 2
    out = _out(args, "run")
    spec = _spec(args, snapshot_binary(args.binary, out))
    spec.census = args.census
    result = run(spec, out / "run")
    report = {"schema": "op1prof.run.v1", "run": result.to_json(),
              "status": "reached" if result.reached else f"failed: {result.error}"}
    if result.reached:
        report["phase"] = {"begin": _phase(args)[0], "end": _phase(args)[1],
                           **trace.phase(trace.load(str(result.trace_path)), *_phase(args)).to_json()}
    (out / "results.json").write_text(json.dumps(report, indent=1))
    for mark in result.marks:
        c = mark.counters
        print(f"{mark.name:14s} wall={mark.wall_ms / 1e3:9.3f}s cpu={mark.cpu_ms / 1e3:9.3f}s "
              f"runs={c['runs']:>12} packets={c['packets']:>13} translated={c['translated']:>7}")
    if not result.reached:
        print(f"run failed: {result.error} (log: {result.directory / 'log.txt'})", file=sys.stderr)
        return 1
    p = report["phase"]
    print(f"{p['begin']} -> {p['end']}: cpu {p['cpu_ms'] / 1e3:.3f} s = compile {p['compile_cpu_ms'] / 1e3:.3f} s"
          f" + execution {p['noncompile_cpu_ms'] / 1e3:.3f} s; wall {p['wall_ms'] / 1e3:.3f} s"
          f" (off-CPU {p['offcpu_ms'] / 1e3:.3f} s) ({out / 'results.json'})")
    return 0


def cmd_verify(args) -> int:
    if not _preflight(args):
        return 2
    out = _out(args, "verify")
    report = verify.verify(_spec(args, snapshot_binary(args.binary, out)), args.runs, out,
                           args.expect, args.write_expect)
    for line in report["differences"]:
        print(line)
    print(f"{report['status']} ({len(report['runs'])} runs; {out / 'results.json'})")
    return 0 if report["status"] == "deterministic" else 1


def cmd_ab(args) -> int:
    if not _preflight(args):
        return 2
    out = _out(args, "ab")
    new_binary = args.new_bin or args.base_bin
    if args.aa:
        if args.new_bin or args.new_arg:
            raise SystemExit("--aa compares the base configuration with itself")
    elif new_binary == args.base_bin and args.base_arg == args.new_arg:
        raise SystemExit("base and new are the same configuration; pass --aa to measure the noise floor")
    base = ab.Side("base", snapshot_binary(args.base_bin, out), list(args.base_arg), sha256_file(args.base_bin))
    new = ab.Side("new", snapshot_binary(new_binary, out), list(args.new_arg if not args.aa else args.base_arg),
                  sha256_file(new_binary))
    report = ab.compare(base, new, _spec(args, base.binary), args.pairs, out, _phase(args),
                        args.min_effect, args.allow_workload_change)
    print(f"status: {report['status']}  ({out / 'results.json'})")
    for line in report.get("unstable", []):
        print(f"unstable: {line}")
    for pair in report["pairs"]:
        for line in pair["workload_diff"]:
            print(f"pair {pair['pair']} workload: {line}")
    for metric, s in report.get("summary", {}).items():
        if "skipped" in s:
            print(f"{metric}: {s['skipped']}")
            continue
        low, high = s["ci95_pct"]
        print(f"{metric}: {s['mean_pct']:+.2f}% (95% CI {low:+.2f}..{high:+.2f}), "
              f"{s['wins']}/{s['pairs']} new faster -> {s['verdict']}")
    return 0 if report["status"] in ("faster", "slower", "inconclusive") else 1


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="op1prof", description="Measure op1emu profiling builds.")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("run", help="one run: marks and the compile/execution split")
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--census", action="store_true", help="also write per-PC/MMIO census (changes timing)")
    _common(p)
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("verify", help="repeat one configuration; its workload must be identical")
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--runs", type=_positive, default=2)
    p.add_argument("--expect", type=Path, help="compare with a saved workload (from --write-expect)")
    p.add_argument("--write-expect", type=Path, help="save the workload when all runs agree")
    _common(p)
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("ab", help="paired AB/BA comparison")
    p.add_argument("--base-bin", required=True, type=Path)
    p.add_argument("--new-bin", type=Path)
    p.add_argument("--base-arg", action="append", default=[])
    p.add_argument("--new-arg", action="append", default=[])
    p.add_argument("--aa", action="store_true", help="compare base with itself: the noise floor")
    p.add_argument("--pairs", type=int, default=4)
    p.add_argument("--min-effect", type=float, default=2.0, help="practical threshold in percent")
    p.add_argument("--allow-workload-change", action="store_true",
                   help="report a verdict even though the sides did different guest work")
    _common(p)
    p.set_defaults(func=cmd_ab)

    args = parser.parse_args(argv)
    return args.func(args)
