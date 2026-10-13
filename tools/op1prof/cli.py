"""op1prof command line: run, verify, ab."""
from __future__ import annotations

import argparse
import json
import sys
import time
from typing import List
from pathlib import Path

from . import ab, census, compilation, diverge, machine, perf, probes, stats, trace, verify
from . import report as perf_report
from .categories import Categories
from .runner import REPO, RunSpec, reserved_args, run, sha256_file, snapshot_binary

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


def _spec(args, binary: Path) -> RunSpec:
    for path in (args.nand, args.otp, args.marks):
        if not path.is_file():
            raise SystemExit(f"not a file: {path}")
    _check_args(args.arg)
    return RunSpec(binary=binary, nand=args.nand.resolve(), otp=args.otp.resolve(),
                   marks=args.marks.resolve(), until=args.until, args=list(args.arg),
                   timeout_s=args.timeout, progress_timeout_s=args.progress_timeout, cpu=args.cpu,
                   counters=getattr(args, "counters", False),
                   checksum_every=getattr(args, "checksum", None), checksum_from=getattr(args, "checksum_from", 0),
                   probes=getattr(args, "probes", False), dump_pcs=getattr(args, "dump_pc", None) or [])


def _phase(args) -> tuple:
    if args.phase:
        begin, _, end = args.phase.partition(":")
        if not end:
            raise SystemExit("--phase must be BEGIN:END")
        return begin, end
    return "start", args.until


def cmd_run(args) -> int:
    _check_args(args.arg)
    if not _preflight(args):
        return 2
    out = _out(args, "run")
    spec = _spec(args, snapshot_binary(args.binary, out))
    spec.census = args.census
    result = run(spec, out / "run")
    report = {"schema": "op1prof.run.v1", "run": result.to_json(),
              "status": "reached" if result.reached else f"failed: {result.error}"}
    if result.reached:
        trace_data = trace.load(str(result.trace_path))
        report["phase"] = {"begin": _phase(args)[0], "end": _phase(args)[1],
                           **trace.phase(trace_data, *_phase(args)).to_json()}
        report["phases"] = [{"begin": a, "end": b, **p.to_json()} for a, b, p in trace.phases(trace_data)]
    (out / "results.json").write_text(json.dumps(report, indent=1))
    for mark in result.marks:
        c = mark.counters
        print(f"{mark.name:14s} wall={mark.wall_ms / 1e3:9.3f}s cpu={mark.cpu_ms / 1e3:9.3f}s "
              f"runs={c['runs']:>12} packets={c['packets']:>13} translated={c['translated']:>7}")
    if not result.reached:
        print(f"run failed: {result.error} (log: {result.directory / 'log.txt'})", file=sys.stderr)
        return 1
    print(f"\n{'phase':28s} {'cpu s':>9} {'compile':>9} {'execution':>9} {'off-CPU':>8} {'transl':>7}")
    for p in report["phases"]:
        print(f"{p['begin'] + ' -> ' + p['end']:28s} {p['cpu_ms'] / 1e3:9.3f} {p['compile_cpu_ms'] / 1e3:9.3f} "
              f"{p['noncompile_cpu_ms'] / 1e3:9.3f} {p['offcpu_ms'] / 1e3:8.3f} {p['translations']:>7}")
    p = report["phase"]
    if p.get("noncompile_insns") is not None:
        print(f"{p['begin']} -> {p['end']}: execution {p['noncompile_insns']:,} host instructions, "
              f"compile {p['compile_insns']:,}")
    other = p.get("other_threads_cpu_ms")
    if other is not None and other > OTHER_THREADS_WARN * p["noncompile_cpu_ms"]:
        print(f"warning: other threads of the emulator used {other / 1e3:.3f} s of CPU, more than "
              f"{OTHER_THREADS_WARN:.0%} of execution CPU; they may compete with the CPU thread", file=sys.stderr)
    print(f"{p['begin']} -> {p['end']}: cpu {p['cpu_ms'] / 1e3:.3f} s = compile {p['compile_cpu_ms'] / 1e3:.3f} s"
          f" + execution {p['noncompile_cpu_ms'] / 1e3:.3f} s; wall {p['wall_ms'] / 1e3:.3f} s"
          f" (off-CPU {p['offcpu_ms'] / 1e3:.3f} s) ({out / 'results.json'})")
    return 0


def cmd_verify(args) -> int:
    _check_args(args.arg)
    if not _preflight(args, compares=True):
        return 2
    out = _out(args, "verify")
    report = verify.verify(_spec(args, snapshot_binary(args.binary, out)), args.runs, out,
                           args.expect, args.write_expect)
    for line in report["differences"]:
        print(line)
    for index, text in report.get("divergence", {}).items():
        print(f"run {index} against run 0:\n{text}")
    if report["differences"] and not args.checksum:
        print("to find where the runs first differ, repeat with --checksum 65536 and run op1prof diverge")
    print(f"{report['status']} ({len(report['runs'])} runs; {out / 'results.json'})")
    return 0 if report["status"] == "deterministic" else 1


def cmd_ab(args) -> int:
    _check_args(args.arg + args.base_arg + args.new_arg)
    if not _preflight(args, compares=True):
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
    try:
        report = ab.compare(base, new, _spec(args, base.binary), args.pairs, out, _phase(args),
                            args.min_effect, args.allow_workload_change)
    except ValueError as error:
        raise SystemExit(str(error))
    return _print_ab(report, out)


def cmd_extend(args) -> int:
    results = args.batch / "results.json"
    try:
        previous = json.loads(results.read_text())
        if "config" not in previous:
            raise ValueError(f"{results} has no batch configuration (written by an older op1prof)")
        base, new, spec = ab.from_config(previous["config"])
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise SystemExit(f"extend: {error}")
    args.cpu, args.arg = spec.cpu, spec.args
    if not _preflight(args, compares=True):
        return 2
    config = previous["config"]
    try:
        report = ab.compare(base, new, spec, args.pairs, args.batch.resolve(), tuple(config["phase"]),
                            config["min_effect_pct"], config["allow_workload_change"], config["seed"], previous)
    except ValueError as error:
        raise SystemExit(f"extend: {error}")
    return _print_ab(report, args.batch)


def _print_ab(report: dict, out: Path) -> int:
    extended = f", extended {report['extensions']} time(s) after its first verdict" if report.get("extensions") else ""
    print(f"status: {report['status']}  ({len(report['pairs'])} pairs{extended}; {out / 'results.json'})")
    for line in report.get("unstable", []):
        print(f"unstable: {line}")
    for pair in report["pairs"]:
        for line in pair["workload_diff"]:
            print(f"pair {pair['pair']} workload: {line}")
        for line in pair.get("contended", []):
            print(f"pair {pair['pair']} contended: {line}")
    for line in report.get("drift", []):
        print(f"drift: {line}")
    for metric, s in report.get("summary", {}).items():
        if "skipped" in s:
            print(f"{metric}: {s['skipped']}")
            continue
        low, high = s["ci95_pct"]
        print(f"{metric}: {s['mean_pct']:+.2f}% (95% CI {low:+.2f}..{high:+.2f}), "
              f"{s['wins']}/{s['pairs']} new faster (sign test p={s['sign_test_p']:.3f}) -> {s['verdict']}")
    codegen = report.get("codegen")
    if codegen:
        b, n = codegen["base"], codegen["new"]
        print(f"generated code: base {b['translations']} translations, {b['ir_insns']:,} IR instructions, "
              f"{b['object_bytes']:,} bytes; new {n['translations']}, {n['ir_insns']:,}, {n['object_bytes']:,}")
        same_config = report["sides"]["base"] == report["sides"]["new"]   # an A/A
        if codegen["identical"] and not same_config:
            # Same code says nothing about how often it ran or which paths it
            # took, so no attribution of the timings follows from it.
            print("observed compile fingerprints are identical: the same blocks compiled to the same machine "
                  "code on both sides. If the change was meant to alter generated code, it did not reach it.")
        if not codegen["repeatable"]:
            print("a side compiled different code in different runs")
    if report.get("decided_on", "noncompile_cpu_ms") != "noncompile_cpu_ms":
        print(f"status decided on {report['decided_on']}: pairs whose core was shared are left out")
    if report.get("summary", {}).get("noncompile_insns"):
        print("noncompile_insns is host work, not time: it never decides the status")
    return 0 if report["status"] in stats.VERDICTS else 1


def cmd_perf(args) -> int:
    _check_args(args.arg)  # perf's own options are appended last, so they win
    begin, _, end = args.window.partition(":")
    problem = perf.check_window(args.marks, (begin, end), args.until) if end else "--window must be BEGIN:END"
    problem = problem or perf.check_call_graph(args.event, args.call_graph)
    problem = problem or perf.preflight(args.event, ["-c", str(args.period)] if args.period else ["-F", str(args.freq)],
                                        args.call_graph)
    if problem:
        print(problem, file=sys.stderr)
        return 2
    if not _preflight(args):
        return 2
    out = _out(args, "perf")
    result = perf.record(_spec(args, snapshot_binary(args.binary, out)), out / "capture", (begin, end),
                         args.event, args.period, args.freq, args.call_graph)
    error = result.error if not result.reached else \
        perf.check_fired(args.marks, (begin, end), [m.name for m in result.marks])
    if error:
        print(f"capture failed: {error} (log: {result.directory / 'log.txt'})", file=sys.stderr)
        return 1
    try:
        print(perf_report.format_report(perf_report.analyze(out / "capture", Categories.load(),
                                                            perf_report.Labels()), args.top))
    except REPORT_ERRORS as error:
        print(f"report: {error}", file=sys.stderr)
        return 1
    return 0


def cmd_annotate(args) -> int:
    try:
        print(perf.annotate(args.capture, args.pc, args.allow_imprecise, args.lines))
    except (OSError, ValueError, KeyError) as error:
        print(f"annotate: {error}", file=sys.stderr)
        return 2
    return 0


def cmd_report(args) -> int:
    try:
        labels = perf_report.Labels(census.Symbols(args.symbols), [perf_report.PcRange.parse(r) for r in args.pc_range])
        data = perf_report.analyze(args.capture, Categories.load(args.categories), labels, args.callers, args.census)
    except REPORT_ERRORS as error:
        print(f"report: {error}", file=sys.stderr)
        return 2
    if args.json:
        args.json.write_text(json.dumps(data, indent=1) + "\n")
    print(perf_report.format_report(data, args.top, args.sort))
    return 0


def _trace_of(run: Path) -> dict:
    return trace.load(str(run / "trace.json" if run.is_dir() else run))


def cmd_probes(args) -> int:
    try:
        print(probes.summary(_trace_of(args.run), args.top))
    except (OSError, ValueError, KeyError) as error:
        print(f"probes: {error}", file=sys.stderr)
        return 2
    return 0


def _thread_names(pairs: List[str]) -> dict:
    names = {}
    for pair in pairs:
        value, _, name = pair.partition("=")
        if not name:
            raise SystemExit(f"--name must be VALUE=NAME, got {pair!r}")
        names[int(value, 16)] = name
    return names


def cmd_threads(args) -> int:
    names = _thread_names(args.name)
    try:
        data = _trace_of(args.run)
        print(probes.threads_report(data, args.probe, args.thread, names))
        if args.perfetto:
            probes.perfetto(data, args.probe, args.thread, names, args.perfetto)
            print(f"guest-time thread tracks: {args.perfetto}")
    except (OSError, ValueError, KeyError) as error:
        print(f"threads: {error}", file=sys.stderr)
        return 2
    return 0


def cmd_diverge(args) -> int:
    try:
        print(diverge.report(args.a, args.b, census.Symbols(args.symbols)))
    except (OSError, ValueError) as error:
        print(f"diverge: {error}", file=sys.stderr)
        return 2
    return 0


def cmd_compile(args) -> int:
    path = args.run / "trace.json"
    try:
        trace_data = trace.load(str(path))
        window = None
        if args.phase:
            begin, _, end = args.phase.partition(":")
            if not end:
                raise SystemExit("--phase must be BEGIN:END")
            window = (begin, end)
        print(compilation.report(trace_data, window, args.top, census.Symbols(args.symbols)))
    except (OSError, ValueError, KeyError) as error:
        print(f"compile: {error}", file=sys.stderr)
        return 2
    return 0


def cmd_census(args) -> int:
    begin, _, end = args.phase.partition(":")
    if not end:
        raise SystemExit("--phase must be BEGIN:END")
    symbols = census.Symbols(args.symbols)
    try:
        if args.edges:
            print(census.edge_report(args.run / "census", begin, end, args.top, symbols))
        elif args.mmio_first:
            from .marks import parse_marks
            fired = {m.name: m for m in parse_marks((args.run / "log.txt").read_text(errors="replace").splitlines())}
            window = (fired[begin].counters["cycles"], fired[end].counters["cycles"])
            print(census.mmio_first_report(args.run / "census", begin, end, window, args.top, symbols))
        else:
            print(census.report(args.run / "census", begin, end, args.top, symbols))
    except (OSError, ValueError, KeyError) as error:
        print(f"census: {error}", file=sys.stderr)
        return 2
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="op1prof", description="Measure op1emu profiling builds.")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("run", help="one run: marks and the compile/execution split")
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--census", action="store_true", help="also write per-PC/MMIO census (changes timing)")
    p.add_argument("--probes", action="store_true", help="capture the marks file's probes (diagnostic)")
    _dump_pc(p)
    _counters(p)
    _checksum(p)
    _common(p)
    p.set_defaults(func=cmd_run)

    p = sub.add_parser("verify", help="repeat one configuration; its workload must be identical")
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--runs", type=_positive, default=2)
    p.add_argument("--expect", type=Path, help="compare with a saved workload (from --write-expect)")
    p.add_argument("--write-expect", type=Path, help="save the workload when all runs agree")
    _counters(p)
    _checksum(p)
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
    _counters(p)
    _common(p)
    p.set_defaults(func=cmd_ab)

    p = sub.add_parser("extend", help="add pairs to an earlier ab batch, with its binaries and settings")
    p.add_argument("batch", type=Path, help="the directory of an op1prof ab batch")
    p.add_argument("--pairs", type=int, default=2, help="pairs to add (even)")
    p.add_argument("--min-mem-gb", type=float, default=10.0, help="refuse to start below this MemAvailable")
    p.add_argument("--force", action="store_true", help="start despite preflight problems (recorded)")
    p.set_defaults(func=cmd_extend)

    p = sub.add_parser("perf", help="perf capture between two marks, reported by guest block")
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--window", required=True, help="BEGIN:END marks enabling perf")
    p.add_argument("--event", default="cpu-clock:u", help="perf event (cycles:upp for instruction-level work; needs a hardware PMU)")
    p.add_argument("--period", type=_positive, help="fixed event count per sample (prefer for precise events)")
    p.add_argument("--freq", type=_positive, default=999, help="samples per second when no --period")
    _dump_pc(p)
    p.add_argument("--call-graph", choices=("fp", "lbr"), default="fp",
                   help="frame pointers, or Intel LBR call stacks (whole chains through code without frame "
                        "pointers; needs a hardware event)")
    p.add_argument("--top", type=int, default=25)
    _common(p)
    p.set_defaults(func=cmd_perf)

    p = sub.add_parser("annotate", help="instruction-level samples of one guest block's generated code")
    p.add_argument("capture", type=Path, help="the capture/ directory of an op1prof perf run")
    p.add_argument("--pc", type=_hex, required=True, help="guest block entry PC (hex)")
    p.add_argument("--allow-imprecise", action="store_true", help="annotate a non-precise capture anyway")
    p.add_argument("--lines", type=int, default=120, help="lines of perf annotate output to show")
    p.set_defaults(func=cmd_annotate)

    p = sub.add_parser("report", help="re-report an earlier perf capture directory")
    p.add_argument("capture", type=Path, help="the capture/ directory of an op1prof perf run")
    p.add_argument("--top", type=int, default=25)
    p.add_argument("--symbols", type=Path, help="ADDRESS NAME lines to name guest blocks")
    p.add_argument("--pc-range", action="append", default=[], metavar="NAME=LO:HI",
                   help="report the blocks in a guest PC range as one row (repeatable)")
    p.add_argument("--categories", type=Path, help="host-path bucket rules (default profiling/categories.json)")
    p.add_argument("--callers", metavar="SYMBOL", help="also break down the callers of a host symbol")
    p.add_argument("--census", type=Path, metavar="RUNDIR",
                   help="run/ directory of an `op1prof run --census` with the same workload: cost per block run")
    p.add_argument("--sort", choices=("seconds", "ns-per-run", "ns-per-packet"), default="seconds",
                   help="order of the census join")
    p.add_argument("--json", type=Path, help="also write the full report as JSON")
    p.set_defaults(func=cmd_report)

    p = sub.add_parser("compile", help="what a run compiled and what each block cost to compile")
    p.add_argument("run", type=Path, help="run directory holding trace.json")
    p.add_argument("--phase", help="BEGIN:END (default the whole run)")
    p.add_argument("--top", type=int, default=25)
    p.add_argument("--symbols", type=Path, help="ADDRESS NAME lines to name PCs")
    p.set_defaults(func=cmd_compile)

    p = sub.add_parser("census", help="per-PC and MMIO counts between two marks of a --census run")
    p.add_argument("run", type=Path, help="run directory holding census.* files")
    p.add_argument("--phase", required=True, help="BEGIN:END")
    p.add_argument("--top", type=int, default=25)
    p.add_argument("--symbols", type=Path, help="ADDRESS NAME lines to name PCs")
    p.add_argument("--edges", action="store_true", help="block-to-block transitions instead of per-block counts")
    p.add_argument("--mmio-first", action="store_true", help="MMRs first accessed in the phase, in order")
    p.set_defaults(func=cmd_census)

    p = sub.add_parser("probes", help="what the probes of a run --probes captured, and the mark reads")
    p.add_argument("run", type=Path, help="run directory (or trace.json)")
    p.add_argument("--top", type=int, default=6, help="most frequent values per field")
    p.set_defaults(func=cmd_probes)

    p = sub.add_parser("threads", help="guest thread timeline from context-switch probes")
    p.add_argument("run", type=Path, help="run directory (or trace.json) of a run --probes")
    p.add_argument("--probe", action="append", required=True, help="a switch probe (repeatable)")
    p.add_argument("--thread", required=True, help="captured value naming the thread switched to, e.g. R1")
    p.add_argument("--name", action="append", default=[], metavar="VALUE=NAME", help="name a thread (hex value)")
    p.add_argument("--perfetto", type=Path, help="also write guest-time thread tracks for Perfetto")
    p.set_defaults(func=cmd_threads)

    p = sub.add_parser("diverge", help="where two runs' guest state first differs (runs made with --checksum)")
    p.add_argument("a", type=Path, help="run directory (or checksum.bin)")
    p.add_argument("b", type=Path)
    p.add_argument("--symbols", type=Path, help="ADDRESS NAME lines to name PCs")
    p.set_defaults(func=cmd_diverge)

    args = parser.parse_args(argv)
    return args.func(args)
