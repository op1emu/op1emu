"""op1prof command line."""
from __future__ import annotations

import argparse
from pathlib import Path

from .analyze import (cmd_annotate, cmd_census, cmd_compile, cmd_diff, cmd_diverge, cmd_perf, cmd_probes,
                      cmd_report, cmd_service, cmd_threads)
from .common import _checksum, _common, _counters, _dump_pc, _hex, _positive
from .measure import _print_ab, cmd_ab, cmd_extend, cmd_gate, cmd_run, cmd_sweep, cmd_verify

__all__ = ["main", "_print_ab"]


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
    p.add_argument("--instrumentation", action="store_true",
                   help="one run per instrumentation setting (census, probes, checksums, counters, no trace): "
                        "none may change the workload")
    _counters(p)
    _checksum(p)
    _common(p)
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("gate", help="the guest's work and state at every mark the same: chained, without "
                                    "chaining, chaining dry, other variants, and an older binary")
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--base-bin", type=Path, help="an older binary whose plain run is the reference")
    p.add_argument("--variant", action="append", default=[], metavar="NAME=ARGS",
                   help="another run of --binary with these emulator arguments (repeatable), "
                        "e.g. no-loops='--no-loop-blocks'")
    _common(p)
    p.set_defaults(func=cmd_gate)

    p = sub.add_parser("sweep", help="one binary with several settings, a few rounds each: compile, worker, "
                                     "execution and wall time per setting")
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--param", action="append", default=[], metavar="FLAG=V1,V2,...",
                   help="an emulator flag (its leading dashes optional) and its values, e.g. "
                        "tier-threshold=3000,30000 (repeatable: every combination)")
    p.add_argument("--config", action="append", default=[], metavar="NAME=ARGS",
                   help="a configuration of its own (repeatable; ARGS split at spaces, may be empty)")
    p.add_argument("--rounds", type=_positive, default=2, help="runs of each configuration (default 2)")
    _counters(p)
    _common(p)
    p.set_defaults(func=cmd_sweep)

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
    p.add_argument("--perf", type=_positive, metavar="N",
                   help="after the timing pairs, N perf captures of each side over the phase, alternating, and "
                        "where their time went (never part of the status)")
    p.add_argument("--perf-event", default="cycles:u", help="perf event for --perf (default cycles:u)")
    p.add_argument("--perf-period", type=_positive, default=4000000, help="event count per sample for --perf")
    p.add_argument("--perf-call-graph", choices=("fp", "lbr"), default="lbr",
                   help="call stacks for --perf (lbr: whole chains, needs a hardware event)")
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
    p.add_argument("--captures", type=_positive, default=1,
                   help="capture N runs and report means and intervals over them (an error estimate needs 3)")
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
    p.add_argument("capture", type=Path, nargs="+",
                   help="capture/ directories of op1prof perf runs (or a directory holding several): with more "
                        "than one, means and intervals over them")
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
    p.add_argument("--min-runs", type=int, default=1000,
                   help="sorting by ns per run or packet, leave out blocks that ran fewer times (their few "
                        "samples rank them by chance)")
    p.add_argument("--json", type=Path, help="also write the full report as JSON")
    p.set_defaults(func=cmd_report)

    p = sub.add_parser("compile", help="what a run compiled and what each block cost to compile")
    p.add_argument("run", type=Path, help="run directory holding trace.json")
    p.add_argument("--phase", help="BEGIN:END (default the whole run)")
    p.add_argument("--top", type=int, default=25)
    p.add_argument("--symbols", type=Path, help="ADDRESS NAME lines to name PCs")
    p.add_argument("--json", type=Path, help="also write the translations and summary as JSON")
    p.set_defaults(func=cmd_compile)

    p = sub.add_parser("census", help="per-PC and MMIO counts between two marks of a --census run")
    p.add_argument("run", type=Path, help="run directory holding census.* files")
    p.add_argument("--phase", required=True, help="BEGIN:END")
    p.add_argument("--top", type=int, default=25)
    p.add_argument("--symbols", type=Path, help="ADDRESS NAME lines to name PCs")
    p.add_argument("--edges", action="store_true", help="block-to-block transitions instead of per-block counts")
    p.add_argument("--mmio-first", action="store_true", help="MMRs first accessed in the phase, in order")
    p.add_argument("--exits", action="store_true",
                   help="after which blocks the host had to act, by what ends the block")
    p.add_argument("--memory", action="store_true", help="guest loads and stores by device and width, and by block")
    p.add_argument("--json", type=Path, help="also write the phase's counts as JSON")
    p.set_defaults(func=cmd_census)

    p = sub.add_parser("service", help="device service calls, the interrupt chain and the event queue of a --census run")
    p.add_argument("run", type=Path, help="run directory holding census.* files")
    p.add_argument("--phase", required=True, help="BEGIN:END")
    p.add_argument("--capture", type=Path,
                   help="capture/ directory of an op1prof perf run (--call-graph lbr) with the same workload: "
                        "time per device call")
    p.add_argument("--top", type=int, default=25)
    p.add_argument("--json", type=Path, help="also write the report as JSON")
    p.set_defaults(func=cmd_service)

    p = sub.add_parser("diff", help="two runs side by side: the guest work counted (census) or the code compiled")
    p.add_argument("what", choices=("census", "compile", "report"))
    p.add_argument("a", type=Path, help="run directory A (the base); for report, captures (one, or a directory of them)")
    p.add_argument("b", type=Path, help="run directory B; for report, captures")
    p.add_argument("--phase", help="BEGIN:END (required for census; compile defaults to the whole run)")
    p.add_argument("--census", type=Path, metavar="RUNDIR",
                   help="compile: weigh blocks by the packets this census run executed in the phase")
    p.add_argument("--top", type=int, default=20)
    p.add_argument("--symbols", type=Path, help="ADDRESS NAME lines to name PCs")
    p.add_argument("--json", type=Path, help="also write the comparison as JSON")
    p.set_defaults(func=cmd_diff)

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
