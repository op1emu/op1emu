"""Commands that run the emulator and judge what it did: run, verify, ab, extend."""
from __future__ import annotations

import json
import sys
from pathlib import Path

from .. import ab, capture_set, gate, perf, stats, sweep, trace, verify
from .. import report as perf_report
from ..categories import Categories
from ..runner import instrument_args, run, sha256_file, snapshot_binary
from .common import OTHER_THREADS_WARN, REPORT_ERRORS, _check_args, _check_timing_args, _out, _phase, _preflight, _spec


def cmd_run(args) -> int:
    _check_args(args.arg)
    phase = _phase(args)
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
        report["phase"] = {"begin": phase[0], "end": phase[1], **trace.phase(trace_data, *phase).to_json()}
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
    if p.get("bg_compile_cpu_ms"):
        estimate = "" if p.get("bg_exact", True) else " (an estimate: the trace has no per-mark worker clock)"
        print(f"{p['begin']} -> {p['end']}: the JIT worker compiled {p['bg_translations']} promotions in "
              f"{p['bg_compile_cpu_ms'] / 1e3:.3f} s of its CPU{estimate}; all JIT CPU {p['jit_cpu_ms'] / 1e3:.3f} s")
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
    if args.phase:
        raise SystemExit("verify compares every mark of every run; --phase does not apply")
    _phase(args)  # --until must name a mark
    if not _preflight(args, compares=True):
        return 2
    if args.expect:
        # Before any run, so a wrong file costs nothing.
        try:
            saved = json.loads(args.expect.read_text())
        except (OSError, ValueError) as error:
            raise SystemExit(f"--expect: {error}")
        if not isinstance(saved, dict) or saved.get("schema") != verify.EXPECT_SCHEMA:
            raise SystemExit(f"--expect: {args.expect} is not an op1prof expect file ({verify.EXPECT_SCHEMA})")
    if args.instrumentation:
        return _verify_instrumentation(args)
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


def cmd_gate(args) -> int:
    _check_args(args.arg)
    phase = _phase(args)
    try:
        variants = [gate.parse_variant(text) for text in args.variant]
    except ValueError as error:
        raise SystemExit(f"--variant: {error}")
    names = [name for name, _ in variants]
    taken = {"base", "plain"} | {name for name, _ in gate.DEFAULT_VARIANTS}
    if len(set(names)) != len(names) or taken & set(names):
        raise SystemExit(f"--variant names must be unique and not {sorted(taken)}")
    for _, extra in variants:
        _check_args(extra)
    if not _preflight(args, compares=True):
        return 2
    out = _out(args, "gate")
    spec = _spec(args, snapshot_binary(args.binary, out))
    base = _spec(args, snapshot_binary(args.base_bin, out)) if args.base_bin else None
    report = gate.gate(spec, out, base, variants, phase)
    print(gate.format_gate(report))
    print(f"({out / 'results.json'})")
    return 0 if report["status"] == "same" else 1


def cmd_sweep(args) -> int:
    _check_args(args.arg)
    phase = _phase(args)
    try:
        configs = sweep.configurations([sweep.parse_param(p) for p in args.param],
                                       [sweep.parse_config(c) for c in args.config])
    except ValueError as error:
        raise SystemExit(f"--param/--config: {error}")
    for _, extra in configs:
        _check_timing_args(extra)
    _check_timing_args(args.arg)
    if not _preflight(args, compares=True):
        return 2
    out = _out(args, "sweep")
    report = sweep.sweep(_spec(args, snapshot_binary(args.binary, out)), out, configs, args.rounds, phase)
    print(sweep.format_sweep(report))
    print(f"({out / 'results.json'})")
    return 0 if report["status"] == "done" else 1


def _verify_instrumentation(args) -> int:
    # Each setting is switched on in one configuration only: an --arg that
    # enabled one everywhere would put it in the plain run as well, and a
    # change it caused would compare equal.
    _check_args(args.arg)
    instrumented = instrument_args(list(args.arg))
    if instrumented:
        raise SystemExit(f"verify --instrumentation switches each setting on by itself and refuses "
                         f"{instrumented} in --arg, which would switch it on in every configuration")
    if args.expect or args.write_expect or args.checksum or args.counters or args.runs != 2:
        raise SystemExit("--instrumentation runs every setting once (checksums and counters included); "
                         "it takes no --runs, --expect, --write-expect, --checksum or --counters")
    out = _out(args, "verify")
    report = verify.instrumentation(_spec(args, snapshot_binary(args.binary, out)), out)
    for entry in report["configurations"]:
        if "skipped" in entry:
            print(f"{entry['name']:9s} skipped: {entry['skipped']}")
        elif "differences" in entry:
            print(f"{entry['name']:9s} {'same workload' if not entry['differences'] else 'DIFFERENT'}")
            for line in entry["differences"]:
                print(f"  {line}")
    print(f"{report['status']} ({len(report['runs'])} runs; {out / 'results.json'})")
    return 0 if report["status"] == "deterministic" else 1


def cmd_ab(args) -> int:
    _check_timing_args(args.arg + args.base_arg + args.new_arg)
    phase = _phase(args)
    if args.perf:
        # Before the timing runs: finding out afterwards costs the batch.
        problem = perf.check_window(args.marks, phase, args.until) or \
            perf.check_call_graph(args.perf_event, args.perf_call_graph) or \
            perf.preflight(args.perf_event, ["-c", str(args.perf_period)], args.perf_call_graph)
        if problem:
            raise SystemExit(f"--perf: {problem}")
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
    spec = _spec(args, base.binary)
    try:
        report = ab.compare(base, new, spec, args.pairs, out, phase, args.min_effect, args.allow_workload_change)
    except ValueError as error:
        raise SystemExit(str(error))
    status = _print_ab(report, out)
    if args.perf and report["status"] in stats.VERDICTS + (ab.WORKLOAD_CHANGED,):
        try:
            a, b = ab.perf_captures(base, new, spec, out, phase, args.perf, args.perf_event, args.perf_period,
                                    args.perf_call_graph)
            categories, labels = Categories.load(), perf_report.Labels()
            data = capture_set.diff_sets(a, capture_set.load(a, categories, labels),
                                         b, capture_set.load(b, categories, labels))
        except REPORT_ERRORS as error:
            print(f"--perf: {error}", file=sys.stderr)
            return 1
        (out / "perf" / "diff.json").write_text(json.dumps(data, indent=1) + "\n")
        print(f"\nwhere the time went, {args.perf} perf captures a side (not part of the status; "
              f"op1prof diff report {out / 'perf' / 'base'} {out / 'perf' / 'new'} repeats it):")
        print(capture_set.format_diff(data, 15))
    return status


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
    shape = report.get("shape")
    if shape:
        keys = [k for k in ("runs", "packets_per_run", "translated", "events", "invalidations")
                if k in shape["base"] and k in shape["new"]]
        cells = []
        for k in keys:
            b, n = shape["base"][k], shape["new"][k]
            cells.append(f"{k} {b:,.2f} -> {n:,.2f}" if isinstance(b, float) else f"{k} {b:,} -> {n:,}")
        print(f"shape at {report['phase'][1]} (base -> new): " + ", ".join(cells))
        if shape["base"].get("runs") != shape["new"].get("runs"):
            print("the sides executed a different number of blocks: the blocks were formed differently")
    if report.get("phases"):
        print(f"{'phase':30s} {'execution':>22} {'compile':>22}   (median delta per pair, mean %; reported, never "
              "a verdict)")
        for row in report["phases"]:
            def cell(m):
                pct = f"{m['mean_pct']:+.1f}%" if m["mean_pct"] is not None else "-"
                return f"{m['median_delta_ms'] / 1e3:+9.3f} s {pct:>9}"
            print(f"{row['begin'] + ' -> ' + row['end']:30s} {cell(row['noncompile_cpu_ms']):>22} "
                  f"{cell(row['compile_cpu_ms']):>22}")
    builds = report.get("builds")
    if builds:
        def revision(build):
            return (f"{build.get('host')}{'+dirty' if build.get('host_dirty') else ''} bcore {build.get('bcore')}"
                    f"{'+dirty' if build.get('bcore_dirty') else ''}")
        print(f"builds: base {revision(builds['base'])}; new {revision(builds['new'])}")
        if builds["kind_differs"]:
            print("warning: the sides are different kinds of build ("
                  + ", ".join(f"{key} {builds['base'].get(key)} vs {builds['new'].get(key)}"
                              for key in builds["kind_differs"])
                  + "): the comparison measures the build settings too", file=sys.stderr)
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
