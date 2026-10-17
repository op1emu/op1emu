"""Tests for tools/op1prof, against a fake emulator (no firmware needed).

Run: python3 -m pytest tools/tests
"""
import json
import os
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from op1prof import ab, capture_set, census, cli, compilation, diff, exits, machine, service, stats, trace, verify  # noqa: E402
from op1prof.marks import parse_mark, workload_diff, parse_marks  # noqa: E402
from op1prof.runner import RESERVED_ARGS, RunSpec, run, snapshot_binary, sha256_file  # noqa: E402

FAKE = Path(__file__).with_name("fake_op1emu.py")
# The marks the fake emulator fires, in this order after "start".
FAKE_MARKS = {"nand_sha256": "0" * 64, "marks": [{"name": "bootrom"}, {"name": "main-display"}],
              "frame": {"name": "main-frame"}}


@pytest.fixture
def files(tmp_path):
    os.chmod(FAKE, os.stat(FAKE).st_mode | stat.S_IXUSR)
    for name in ("nand.bin", "otp.bin"):
        (tmp_path / name).write_text("x")
    (tmp_path / "marks.json").write_text(json.dumps(FAKE_MARKS))
    return tmp_path


def spec(files, **kw):
    base = dict(binary=FAKE, nand=files / "nand.bin", otp=files / "otp.bin",
                marks=files / "marks.json", until="main-frame", timeout_s=30, progress_timeout_s=5)
    base.update(kw)
    return RunSpec(**base)


# ---- marks ----

def test_parse_mark_line():
    m = parse_mark("[mark] main-display pc=0x019ab8b6 wall_ms=1.5 cpu_ms=1.25 runs=3 packets=10 "
                   "cycles=10 translated=2 mmio_r=1 mmio_w=0 events=4 deterministic=1")
    assert m.name == "main-display" and m.pc == 0x019ab8b6 and m.deterministic
    assert m.workload() == {"packets": 10, "cycles": 10, "mmio_r": 1, "mmio_w": 0}  # events is shape
    assert parse_mark("unrelated line") is None


def test_malformed_and_duplicate_marks_are_errors():
    with pytest.raises(ValueError):
        parse_mark("[mark] x pc=zz wall_ms=1 cpu_ms=1")
    line = "[mark] a pc=0x0 wall_ms=0 cpu_ms=0 packets=1 cycles=1 mmio_r=0 mmio_w=0 events=0 runs=1 translated=0"
    with pytest.raises(ValueError):
        parse_marks([line, line])


def test_workload_diff_ignores_shape_but_not_work():
    def mark(packets, runs):
        return parse_mark(f"[mark] a pc=0x0 wall_ms=0 cpu_ms=0 runs={runs} packets={packets} cycles={packets} "
                          "translated=1 mmio_r=0 mmio_w=0 events=0")
    assert workload_diff([mark(100, 5)], [mark(100, 9)]) == []  # longer blocks: fewer runs, same work
    assert workload_diff([mark(100, 5)], [mark(90, 5)])         # less work


# ---- trace ----

def _trace(events, dropped=0):
    return {"schema": "op1.trace.v2", "dropped": dropped, "traceEvents": events}


def _mark(name, ts_us, wall_ns, cpu_ns):
    return {"name": name, "cat": "mark", "ph": "i", "ts": ts_us, "args": {"wall_ns": wall_ns, "thread_cpu_ns": cpu_ns}}


def _translate(ts_us, dur_us, cpu_ns):
    return {"name": "translate", "cat": "jit", "ph": "X", "ts": ts_us, "dur": dur_us, "args": {"cpu_ns": cpu_ns}}


def test_phase_splits_compile_from_execution():
    t = _trace([_mark("a", 0, 0, 0), _translate(10, 20, 15000), _translate(20, 20, 15000),
                _mark("b", 1000, 1_000_000, 900_000)])
    p = trace.phase(t, "a", "b")
    assert p.wall_ms == 1.0 and p.cpu_ms == 0.9
    assert p.compile_wall_ms == pytest.approx(0.030)   # union of overlapping spans, not their sum
    assert p.compile_cpu_ms == pytest.approx(0.030)
    assert p.noncompile_cpu_ms == pytest.approx(0.870)
    assert p.offcpu_ms == pytest.approx(0.1)


def test_trace_rejects_loss_and_inverted_or_straddled_windows(tmp_path):
    path = tmp_path / "t.json"
    path.write_text(json.dumps(_trace([], dropped=3)))
    with pytest.raises(trace.TraceError):
        trace.load(str(path))
    t = _trace([_mark("a", 0, 0, 0), _translate(990, 20, 1), _mark("b", 1000, 1, 1)])
    with pytest.raises(trace.TraceError):
        trace.phase(t, "a", "b")
    with pytest.raises(trace.TraceError):
        trace.phase(_trace([_mark("a", 10, 0, 0), _mark("b", 0, 0, 0)]), "a", "b")


# ---- stats ----

def test_paired_uses_actual_mean_and_counts_wins():
    c = stats.paired([100, 100, 100, 100], [90, 92, 91, 89])
    assert c.mean_pct == pytest.approx(-9.5)
    assert c.ci_low_pct <= c.mean_pct <= c.ci_high_pct
    assert c.wins == 4 and c.verdict(2.0) == "faster"
    assert stats.paired([100, 100, 100, 100], [99, 101, 100, 100]).verdict(2.0) == "inconclusive"


# ---- runner ----

def test_run_reaches_mark_with_clean_environment(files, monkeypatch):
    monkeypatch.setenv("OP1_LEAK", "1")
    monkeypatch.setenv("FAKE_ENVLOG", str(files / "env.json"))
    result = run(spec(files), files / "run")
    assert result.reached and result.returncode == 0
    assert [m.name for m in result.marks] == ["start", "bootrom", "main-display", "main-frame"]
    assert "OP1_LEAK" not in json.loads((files / "env.json").read_text())
    assert (result.directory / "otp.bin").is_file() and (result.directory / "gui").is_symlink()
    assert "--deterministic" in result.command


def test_hung_run_is_killed_and_reported(files, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "hang")
    result = run(spec(files, progress_timeout_s=2), files / "run")
    assert not result.reached and "no new mark" in result.error
    assert result.returncode is not None  # reaped, not left running


def test_failed_exit_is_an_error(files, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "fail")
    result = run(spec(files), files / "run")
    assert not result.reached and result.error == "exit status 3"


def test_closed_output_cannot_outlast_the_deadline(files, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "closeout")
    result = run(spec(files, timeout_s=3), files / "run")
    assert not result.reached and result.error == "closed its output but did not exit"
    assert result.elapsed_s < 30 and result.returncode is not None  # killed and reaped


def test_extra_args_cannot_end_the_options(files):
    with pytest.raises(ValueError):
        spec(files, args=["--", "x.ldr"])


@pytest.mark.parametrize("extra", [["--nand-rw"], ["--profile-until", "bootrom"]])
def test_extra_args_cannot_override_the_runner(files, tmp_path, extra):
    with pytest.raises(ValueError):
        spec(files, args=extra)
    with pytest.raises(SystemExit):
        _cli(files, "run", "--out", str(tmp_path / "r"), *(f"--arg={a}" for a in extra))
    with pytest.raises(SystemExit):
        cli.main(["ab", "--base-bin", str(FAKE), "--nand", str(files / "nand.bin"), "--otp", str(files / "otp.bin"),
                  "--marks", str(files / "marks.json"), "--force", "--out", str(tmp_path / "a"),
                  *(f"--new-arg={a}" for a in extra)])
    assert not (tmp_path / "r").exists() and not (tmp_path / "a").exists()  # refused before starting


def test_extra_args_still_vary_the_rest(files):
    assert spec(files, args=["--rtc-epoch", "0", "--profile-census", "census"]).args
    assert "--perf-window" not in RESERVED_ARGS  # added by the perf command itself


def test_snapshot_binary_is_content_named(files, tmp_path):
    copy = snapshot_binary(FAKE, tmp_path / "out")
    assert copy.name == f"op1emu-{sha256_file(FAKE)[:12]}" and os.access(copy, os.X_OK)


def _cli(files, command, *extra):
    return cli.main([command, "--binary", str(FAKE), "--nand", str(files / "nand.bin"),
                     "--otp", str(files / "otp.bin"), "--marks", str(files / "marks.json"),
                     "--force", "--timeout", "30", "--progress-timeout", "5", *extra])


def test_cli_run_with_relative_out_records_results(files, monkeypatch):
    # The default --out is relative; the run starts the copied binary from
    # inside its own directory, so that copy must be addressed absolutely.
    monkeypatch.chdir(files)
    assert _cli(files, "run", "--out", "rel") == 0
    report = json.loads((files / "rel" / "results.json").read_text())
    assert report["status"] == "reached" and Path(report["run"]["command"][0]).is_absolute()
    assert report["phase"]["begin"] == "start" and report["phase"]["noncompile_cpu_ms"] > 0


def test_cli_failed_run_keeps_its_results(files, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "fail")
    assert _cli(files, "run", "--out", str(files / "out")) == 1
    report = json.loads((files / "out" / "results.json").read_text())
    assert report["status"] == "failed: exit status 3" and "phase" not in report
    assert report["run"]["machine_before"] and report["run"]["machine_after"]


def test_verify_needs_a_run(files, tmp_path):
    with pytest.raises(SystemExit):
        _cli(files, "verify", "--runs", "0", "--out", str(tmp_path / "v"))
    with pytest.raises(ValueError):
        verify.verify(spec(files), 0, tmp_path, None, None)



def test_verify_records_inputs_and_checks_them_against_an_expect_file(files, tmp_path):
    expect = tmp_path / "expect.json"
    report = verify.verify(spec(files), 1, tmp_path / "v1", None, expect)
    inputs = report["runs"][0]["inputs"]
    assert report["status"] == "deterministic" and set(inputs) == {"nand_sha256", "otp_sha256", "marks_sha256"}
    assert json.loads(expect.read_text())["inputs"] == inputs
    (files / "otp.bin").write_text("another unit")  # same workload from the fake, different image
    report = verify.verify(spec(files), 1, tmp_path / "v2", expect, None)
    assert report["status"] == "differences found"
    assert any("input otp_sha256" in line for line in report["differences"])



def test_input_hashes_follow_content_not_file_metadata(files):
    from op1prof.runner import input_hashes
    first = input_hashes(spec(files))["otp_sha256"]
    otp = files / "otp.bin"
    info = os.stat(otp)
    otp.write_text("y")                                   # same size
    os.utime(otp, ns=(info.st_atime_ns, info.st_mtime_ns))  # same mtime
    assert input_hashes(spec(files))["otp_sha256"] != first


# ---- ab ----

def _side(name, files, out, args=()):
    return ab.Side(name, snapshot_binary(FAKE, out), list(args), sha256_file(FAKE))


def test_ab_detects_a_faster_side(files, monkeypatch, tmp_path):
    out = tmp_path / "ab"
    out.mkdir()
    # The fake reads its speed from the environment; give "new" a faster one
    # by wrapping its binary.
    fast = tmp_path / "fast.sh"
    fast.write_text(f"#!/bin/sh\nFAKE_SCALE=0.8 exec {FAKE} \"$@\"\n")
    fast.chmod(0o755)
    base = _side("base", files, out)
    new = ab.Side("new", snapshot_binary(fast, out), [], sha256_file(fast))
    report = ab.compare(base, new, spec(files), 4, out, ("start", "main-frame"), 2.0, False)
    assert report["status"] == "faster", report["status"]
    s = report["summary"]["noncompile_cpu_ms"]
    assert s["mean_pct"] == pytest.approx(-20.0) and s["wins"] == 4
    assert [p["order"] for p in report["pairs"]][:2] == [["new", "base"], ["base", "new"]]
    assert json.loads((out / "results.json").read_text())["status"] == "faster"


def test_ab_gates_on_workload(files, tmp_path):
    out = tmp_path / "ab"
    out.mkdir()
    less = tmp_path / "less.sh"
    less.write_text(f"#!/bin/sh\nFAKE_WORK=900 FAKE_SCALE=0.8 exec {FAKE} \"$@\"\n")
    less.chmod(0o755)
    report = ab.compare(_side("base", files, out), ab.Side("new", snapshot_binary(less, out), [], "x"),
                        spec(files), 4, out, ("start", "main-frame"), 2.0, False)
    # Faster, but because it did less work: no verdict.
    assert report["status"].startswith("workload changed")
    assert report["pairs"][0]["workload_diff"]


def test_ab_stops_without_verdict_on_failure(files, monkeypatch, tmp_path):
    out = tmp_path / "ab"
    out.mkdir()
    monkeypatch.setenv("FAKE_MODE", "fail")
    report = ab.compare(_side("base", files, out), _side("new", files, out), spec(files), 4, out,
                        ("start", "main-frame"), 2.0, False)
    assert report["status"].startswith("failed") and len(report["runs"]) == 1


def test_ab_flags_nondeterministic_configuration(files, monkeypatch, tmp_path):
    out = tmp_path / "ab"
    out.mkdir()
    monkeypatch.setenv("FAKE_MODE", "noise")
    report = ab.compare(_side("base", files, out), _side("new", files, out), spec(files), 4, out,
                        ("start", "main-frame"), 2.0, True)
    assert report["status"].startswith("unusable") and report["unstable"]


def test_ab_requires_balanced_pairs(files, tmp_path):
    with pytest.raises(ValueError):
        ab.compare(None, None, spec(files), 3, tmp_path, ("start", "main-frame"), 2.0, False)


# ---- perf ----

from op1prof import census, perf  # noqa: E402
from op1prof import report as perf_report  # noqa: E402
from op1prof.categories import Categories  # noqa: E402


def test_precise_events():
    assert perf.is_precise("cycles:pp") and perf.is_precise("cycles:p")
    assert not perf.is_precise("cpu-clock:u") and not perf.is_precise("cycles")


def test_window_must_be_in_mark_order(tmp_path):
    marks = tmp_path / "marks.json"
    marks.write_text(json.dumps({"marks": [{"name": "a"}, {"name": "b"}], "frame": {"name": "f"}}))
    assert perf.check_window(marks, ("a", "f"), "f") is None
    assert perf.check_window(marks, ("start", "a"), "b") is None
    assert "empty" in perf.check_window(marks, ("b", "a"), "f")
    assert "unknown" in perf.check_window(marks, ("a", "zz"), "f")
    assert "unknown" in perf.check_window(marks, ("a", "b"), "zz")
    assert "before the window end" in perf.check_window(marks, ("a", "f"), "b")


def test_fired_marks_must_follow_file_order(tmp_path):
    # check_window() trusts file order; a capture shows whether the guest kept it.
    marks = tmp_path / "marks.json"
    marks.write_text(json.dumps({"marks": [{"name": "a"}, {"name": "b"}, {"name": "c"}], "frame": {"name": "f"}}))
    assert perf.check_fired(marks, ("a", "c"), ["start", "a", "b", "c", "f"]) is None
    assert perf.check_fired(marks, ("a", "c"), ["start", "a", "c"]) is None  # b need not fire
    assert "out of file order" in perf.check_fired(marks, ("a", "c"), ["start", "c", "a"])
    assert "out of file order" in perf.check_fired(marks, ("a", "b"), ["start", "a", "f", "b"])
    assert "did not fire: ['c']" in perf.check_fired(marks, ("a", "c"), ["start", "a", "b"])


def test_perf_fails_when_the_window_never_closed(files, monkeypatch, capsys):
    # lap-end never fires: the run reaches --until, but the window it was
    # meant to sample did not close, so there is nothing to report.
    monkeypatch.setenv("FAKE_SKIP", "lap-end")
    marks = files / "window.json"
    marks.write_text(json.dumps({"marks": [{"name": "bootrom"}, {"name": "lap-end"}, {"name": "main-display"}],
                                 "frame": {"name": "main-frame"}}))
    monkeypatch.setattr(perf, "preflight", lambda *a: None)
    monkeypatch.setattr(perf, "record", lambda spec, directory, *a: run(spec, directory))
    monkeypatch.setattr(perf_report, "analyze", lambda *a: pytest.fail("reported an unclosed window"))
    assert cli.main(["perf", "--binary", str(FAKE), "--nand", str(files / "nand.bin"), "--otp", str(files / "otp.bin"),
                     "--marks", str(marks), "--window", "bootrom:lap-end", "--force",
                     "--out", str(files / "p"), "--timeout", "30", "--progress-timeout", "5"]) == 1
    assert "window marks did not fire: ['lap-end']" in capsys.readouterr().err


RUN = "BlackFinCpu::Run() (/x/op1emu)"


def _script(*samples):
    """perf script text: (tid, seconds, period, [frame, ...]) with frames leaf first."""
    lines = []
    for tid, seconds, period, frames in samples:
        lines.append(f" {tid} {seconds}:     {period} ")
        lines += [f"\t    {0x1000 + i:x} {frame}" for i, frame in enumerate(frames)]
        lines.append("")
    return lines


def test_parse_samples_reads_call_chains():
    lines = _script((42, "10.000000100", 250, ["bb_0x01a5e27e (/tmp/jitted-42-1.so)",
                                                "Core::run(unsigned int) (/x/op1emu)", RUN]))
    lines += [" 43 10.5:     7 ", "\t    7f00 [unknown] ([unknown])"]   # last sample, no blank line
    first, last = perf_report.parse_samples(lines)
    assert (first.tid, first.time_ns, first.period) == (42, 10_000_000_100, 250)
    assert first.frames == (("bb_0x01a5e27e", "jitted-42-1.so"), ("Core::run(unsigned int)", "op1emu"),
                            ("BlackFinCpu::Run()", "op1emu"))
    assert last.frames == (("[unknown]", "[unknown]"),) and last.period == 7


def _leaves(*samples):
    """perf script -G text: (tid, seconds, period, frame), the frame of the sampled IP."""
    return [f" {tid} {seconds}:     {period}     {0x2000 + i:x} {frame}"
            for i, (tid, seconds, period, frame) in enumerate(samples)]


# Core::run called bb_0x01a9bd30, which ran bb_0x019be3f0 by a musttail
# jump: the LBR call stack's leaf is the target of the last call, the
# sampled IP is in the block jumped to.
CHAINED = [
    (42, "10.000000100", 250, ["bb_0x01a9bd30 (/j.so)", "Core::run(unsigned int) (/x/op1emu)", RUN]),
    (42, "10.000000200", 50, ["EmulatorMemory::read16(unsigned int) const (/x/op1emu)", "bb_0x019be3f0 (/j.so)",
                              "Core::run(unsigned int) (/x/op1emu)", RUN]),
]
CHAINED_LEAVES = [
    (42, "10.000000100", 250, "bb_0x019be3f0 (/j.so)"),
    (42, "10.000000200", 50, "EmulatorMemory::read16(unsigned int) const (/x/op1emu)"),
]


def test_lbr_leaf_is_the_sampled_ip_not_the_block_the_chain_was_entered_by():
    first, second = perf_report.with_leaves(perf_report.parse_samples(_script(*CHAINED)),
                                            perf_report.parse_leaves(_leaves(*CHAINED_LEAVES)))
    assert first.frames == (("bb_0x019be3f0", "j.so"), ("Core::run(unsigned int)", "op1emu"),
                            ("BlackFinCpu::Run()", "op1emu"))
    assert second.frames == next(perf_report.parse_samples(_script(CHAINED[1]))).frames
    result = perf_report.attribute([first, second], _perf_trace(), ("a", "b"), Categories.load(),
                                   perf_report.Labels())
    assert result.self_blocks == {"bb_0x019be3f0": 250}
    assert result.incl_blocks == {"bb_0x019be3f0": 300}


def test_leaves_must_be_the_same_samples_in_the_same_order():
    chains = perf_report.parse_samples(_script(*CHAINED))
    other = [CHAINED_LEAVES[0], (42, "10.000000200", 51, CHAINED_LEAVES[1][3])]
    with pytest.raises(ValueError, match="sample 1"):
        list(perf_report.with_leaves(chains, perf_report.parse_leaves(_leaves(*other))))
    with pytest.raises(ValueError, match="2 samples with call chains, 1 without"):
        list(perf_report.with_leaves(perf_report.parse_samples(_script(*CHAINED)),
                                     perf_report.parse_leaves(_leaves(CHAINED_LEAVES[0]))))
    with pytest.raises(ValueError, match="cannot parse"):
        list(perf_report.parse_leaves([" 42 10.000000100:     250 "]))


@pytest.mark.parametrize("call_graph, leaf", [("lbr", "bb_0x019be3f0"), ("fp", "bb_0x01a9bd30")])
def test_analyze_takes_leaves_from_the_sampled_ip_for_lbr_captures(tmp_path, monkeypatch, call_graph, leaf):
    capture = _capture(tmp_path, _perf_trace())
    info = json.loads((capture / "capture.json").read_text())
    (capture / "capture.json").write_text(json.dumps({**info, "call_graph": call_graph}))
    passes = []

    def script(data, call_chains=True):
        passes.append(call_chains)
        return _script(*CHAINED) if call_chains else _leaves(*CHAINED_LEAVES)

    monkeypatch.setattr(perf_report, "_perf_script", script)
    data = perf_report.analyze(capture, Categories.load(), perf_report.Labels())
    assert data["blocks"][0]["block"] == leaf
    # Frame-pointer chains start at the sampled IP already: one pass.
    assert passes == ([True, False] if call_graph == "lbr" else [True])


def _perf_trace(tid=42):
    return {"schema": "op1.trace.v2", "dropped": 0, "cpu_thread_tid": tid, "traceEvents": [
        {"name": "a", "cat": "mark", "ts": 10_000_000.0, "args": {"wall_ns": 0, "thread_cpu_ns": 0}},
        {"name": "translate", "ph": "X", "ts": 10_000_000.5, "dur": 0.1, "args": {"cpu_ns": 100}},
        {"name": "b", "cat": "mark", "ts": 10_000_001.0, "args": {"wall_ns": 1000, "thread_cpu_ns": 1000}},
    ]}


SAMPLES = [
    (42, "10.000000100", 100, ["bb_0x01a5e27e (/j.so)", "Core::run(unsigned int) (/x/op1emu)", RUN]),
    (42, "10.000000200", 300, ["NFC::Read(unsigned int) (/x/op1emu)", "EmulatorMemory::read16(unsigned int) (/x/op1emu)",
                               "bb_0x01a5e27e (/j.so)", "Core::run(unsigned int) (/x/op1emu)", RUN]),
    (42, "10.000000300", 50, ["memcpy (/lib/libc.so.6)", "BlackFinCpu::ProcessEvents() (/x/op1emu)", RUN]),
    (42, "10.000000400", 50, ["memcpy (/lib/libc.so.6)", "[unknown] ([unknown])"]),          # broken chain
    (42, "10.000000550", 999, ["bb_0x00000000 (/j.so)", RUN]),                               # translating
    (43, "10.000000300", 999, ["glfwPollEvents (/x/libglfw.so)"]),                           # another thread
    (42, "11.000000000", 999, ["bb_0x01a5e27e (/j.so)", RUN]),                               # after the window
]


def test_attribution_weights_buckets_and_inclusive_time():
    samples = list(perf_report.parse_samples(_script(*SAMPLES)))
    result = perf_report.attribute(samples, _perf_trace(), ("a", "b"), Categories.load(), perf_report.Labels())
    assert result.counts == {"execution": 4, "compile": 1, "other_threads": 1, "outside_window": 1}
    assert result.execution == 500 and result.complete == 450
    # The device read is device time even though generated code called it.
    assert result.buckets == {"generated code": 100, "nand": 300, "event queue": 50, "uncategorized": 50}
    assert result.self_blocks == {"bb_0x01a5e27e": 100}
    assert result.incl_blocks == {"bb_0x01a5e27e": 400}
    assert result.incl_symbols["BlackFinCpu::Run()"] == 450 and result.incl_symbols["NFC::Read(unsigned int)"] == 300
    assert result.self_symbols["memcpy (libc.so.6)"] == 100


def test_overlapping_translate_spans_are_merged():
    assert perf_report._merged([(10, 20), (0, 100), (150, 160), (155, 170)]) == [(0, 100), (150, 170)]


def test_buckets_prefer_the_nearest_frame_then_rule_order():
    categories = Categories.load()
    assert categories.bucket(["DMABus::Transfer()", "Bus::Read()"]) == "dma"       # rule order within a frame
    assert categories.bucket(["std::_Hash_bytes", "JitEngine::lookup(unsigned int)"]) == "dispatch"
    assert categories.bucket(["libfoo"]) == "uncategorized"
    assert categories.bucket(["Bus::Read()"]) == "memory callbacks"
    assert categories.bucket(["OTG_USB::Poll()"]) == "uncategorized"     # patterns start a name
    assert categories.bucket(["std::function<void ()>::operator()", "DummyI2CPeripheral::Read()"]) == "gpio/twi/sensors"
    assert not categories.complete(["memcpy", "[unknown]"])



def test_a_devices_own_register_reads_are_its_time_and_guest_reads_are_memory_callbacks():
    categories = Categories.load()
    lookup = ["std::_Rb_tree<unsigned int, ...>::find(unsigned int const&)", "RegisterDevice::Read32(unsigned int)"]
    # The RTC polling its own registers after every block.
    assert categories.bucket(lookup + ["RTC::UpdateInterrupts()", "RTC::ProcessWithInterrupt(int)",
                                       "BlackFinCpu::Run()"]) == "rtc"
    # Generated code reading an MMR: the dispatch is the memory path.
    assert categories.bucket(lookup + ["Emulator::MemoryRead32(unsigned int)", "EmulatorMemory::read32(unsigned int) const",
                                       "mem_read32", "bb_0x01a5e27e", "BlackFinCpu::Run()"]) == "memory callbacks"
    assert categories.bucket(["mem_read16", "bb_0x01a5e27e"]) == "memory callbacks"


def _capture(tmp_path, trace_data, marks=()):
    capture = tmp_path / "capture"
    (capture / "run").mkdir(parents=True)
    (capture / "run" / "trace.json").write_text(json.dumps(trace_data))
    run_entry = {"marks": list(marks), "binary_sha256": "b" * 64, "inputs": {"nand_sha256": "n" * 64},
                 "guest_args": [], "script_sha256": "s" * 64}
    (capture / "capture.json").write_text(json.dumps({
        "event": "cpu-clock:u", "precise": False, "rate": ["-F", "999"], "window": ["a", "b"],
        "run": run_entry}))
    (capture / "run" / "run.json").write_text(json.dumps(run_entry))
    return capture


def _census_run_entry(census_run, marks, binary="b" * 64, nand="n" * 64):
    """The log and run.json a census run of op1prof writes beside its dumps."""
    (census_run / "log.txt").write_text("\n".join(map(_mark_line, marks)) + "\n")
    (census_run / "run.json").write_text(json.dumps({"marks": list(marks), "binary_sha256": binary,
                                                     "inputs": {"nand_sha256": nand}, "guest_args": [],
                                                     "script_sha256": "s" * 64}))


def test_report_withholds_inclusive_tables_for_broken_chains(tmp_path):
    capture = _capture(tmp_path, _perf_trace())
    broken = [(42, "10.000000100", 10, ["bb_0x01a5e27e (/j.so)", RUN]),
              (42, "10.000000200", 90, ["memcpy (/lib/libc.so.6)"])]
    data = perf_report.analyze(capture, Categories.load(), perf_report.Labels(), callers_of="memcpy",
                               samples=perf_report.parse_samples(_script(*broken)))
    assert data["chain_complete_pct"] == 10.0 and not data["inclusive_valid"]
    assert data["host_inclusive"] == [] and data["callers"]["rows"] == []
    assert data["buckets"]  # still reported
    # cpu-clock periods are CPU nanoseconds: 100 ns sampled vs 1000 - 100 ns of execution
    assert data["sampled_over_trace"] == pytest.approx(100 / 900)
    text = perf_report.format_report(data, 10)
    assert "withheld" in text and "not a precise event" in text
    assert data["blocks"][0]["incl_s"] is None and data["blocks"][0]["self_s"] == pytest.approx(0.1 * 900e-9)
    with pytest.raises(ValueError, match="whole call chains"):
        perf_report.analyze(capture, Categories.load(), perf_report.Labels(), census_run=tmp_path,
                            samples=perf_report.parse_samples(_script(*broken)))


def _mark_entry(name, packets):
    return {"name": name, "pc": 0, "wall_ms": 0.0, "cpu_ms": 0.0, "runs": 1, "packets": packets, "cycles": packets,
            "translated": 1, "mmio_r": 0, "mmio_w": 0, "events": 0}


def _mark_line(entry):
    fields = " ".join(f"{k}={v}" for k, v in entry.items() if k not in ("name", "pc"))
    return f"[mark] {entry['name']} pc=0x0 {fields}"


def test_census_join_prices_block_runs_and_refuses_other_work(tmp_path):
    marks = [_mark_entry("a", 10), _mark_entry("b", 20)]
    capture = _capture(tmp_path, _perf_trace(), marks)
    census_run = tmp_path / "census-run"
    census_run.mkdir()
    _census_run_entry(census_run, marks)
    _census(census_run / "census.pc.a", [(0x01a5e27e, 1, 4)])
    _census(census_run / "census.pc.b", [(0x01a5e27e, 5, 24), (0x200, 3, 3)])
    _census(census_run / "census.mmio.a", [])
    _census(census_run / "census.mmio.b", [])
    whole = [sample for sample in SAMPLES if "[unknown]" not in sample[3][-1]]
    whole.append((42, "10.000000600", 25, ["bb_0x00000300 (/j.so)", RUN]))   # a block the census never ran
    samples = list(perf_report.parse_samples(_script(*whole)))
    labels = perf_report.Labels(ranges=[perf_report.PcRange.parse("fir=01a5e240:01a5e290")])
    data = perf_report.analyze(capture, Categories.load(), labels, census_run=census_run, samples=samples)
    rows = {row["block"]: row for row in data["census"]["blocks"]}
    per_weight = 900e-9 / 475
    assert data["census"]["sampled_not_in_census_s"] == pytest.approx(25 * per_weight)
    assert rows["fir"]["runs"] == 4 and rows["fir"]["packets"] == 20
    assert rows["fir"]["ns_per_run"] == pytest.approx(400 * per_weight * 1e9 / 4)
    assert rows["bb_0x00000200"]["seconds"] == 0
    assert data["census"]["outside_blocks_ns_per_run"] == pytest.approx(50 * per_weight * 1e9 / 7)
    assert "ns/run" in perf_report.format_report(data, 10, "ns-per-run")
    # With an exit census the join also gives each block's memory accesses.
    _exits(census_run / "census.exit.a", {0x01a5e27e: (0, 0, 0, 2)})
    _exits(census_run / "census.exit.b", {0x01a5e27e: (0, 0, 0, 10), 0x200: (0, 0, 0, 6)})
    with_accesses = perf_report.analyze(capture, Categories.load(), labels, census_run=census_run, samples=samples)
    fir = {row["block"]: row for row in with_accesses["census"]["blocks"]}["fir"]
    assert fir["accesses_per_run"] == pytest.approx(8 / 4) and fir["ns_per_access"] == pytest.approx(fir["seconds"] * 1e9 / 8)
    assert "acc/run" in perf_report.format_report(with_accesses, 10)
    for mark in "ab":
        (census_run / f"census.exit.{mark}").unlink()
    _census_run_entry(census_run, [marks[0], _mark_entry("b", 21)])
    with pytest.raises(ValueError, match="different guest work"):
        perf_report.analyze(capture, Categories.load(), labels, census_run=census_run, samples=samples)
    # The same work in other blocks: the census divides by its own block
    # counts, so it must have run the same blocks.
    _census_run_entry(census_run, [marks[0], {**marks[1], "runs": 2}])
    with pytest.raises(ValueError, match="another number of blocks"):
        perf_report.analyze(capture, Categories.load(), labels, census_run=census_run, samples=samples)
    _census_run_entry(census_run, marks, binary="c" * 64)
    with pytest.raises(ValueError, match="another binary"):
        perf_report.analyze(capture, Categories.load(), labels, census_run=census_run, samples=samples)
    _census_run_entry(census_run, marks, nand="m" * 64)
    with pytest.raises(ValueError, match="another input"):
        perf_report.analyze(capture, Categories.load(), labels, census_run=census_run, samples=samples)
    (census_run / "run.json").unlink()
    with pytest.raises(ValueError, match="older op1prof"):
        perf_report.analyze(capture, Categories.load(), labels, census_run=census_run, samples=samples)


def test_pc_range_and_symbol_labels(tmp_path):
    assert perf_report.PcRange.parse("fir=0x10:0x20") == perf_report.PcRange("fir", 0x10, 0x20)
    with pytest.raises(ValueError):
        perf_report.PcRange.parse("fir")
    with pytest.raises(ValueError, match="LO above HI"):
        perf_report.PcRange.parse("fir=0x20:0x10")
    symbols = tmp_path / "syms.txt"
    symbols.write_text("00000100 dsp_fir\n")
    labels = perf_report.Labels(census.Symbols(symbols), [perf_report.PcRange("loop", 0x200, 0x2ff)])
    assert labels(0x104) == "bb_0x00000104 dsp_fir+0x4" and labels(0x250) == "loop"
    assert perf_report.block_pc("bb_0x01a5e27e") == 0x01a5e27e and perf_report.block_pc("bb_main") is None


def test_phases_cover_every_pair_of_consecutive_marks():
    data = {"traceEvents": [
        {"name": "start", "cat": "mark", "ts": 0.0, "args": {"wall_ns": 0, "thread_cpu_ns": 0}},
        {"name": "translate", "ph": "X", "ts": 1.0, "dur": 2.0, "args": {"cpu_ns": 1500}},
        {"name": "a", "cat": "mark", "ts": 5.0, "args": {"wall_ns": 5000, "thread_cpu_ns": 4000}},
        {"name": "translate", "ph": "X", "ts": 6.0, "dur": 1.0, "args": {"cpu_ns": 1000}},
        {"name": "translate", "ph": "X", "ts": 7.0, "dur": 1.0, "args": {"cpu_ns": 1000}},
        {"name": "b", "cat": "mark", "ts": 9.0, "args": {"wall_ns": 9000, "thread_cpu_ns": 8000}},
    ]}
    table = trace.phases(data)
    assert [(a, b) for a, b, _ in table] == [("start", "a"), ("a", "b")]
    assert [p.translations for _, _, p in table] == [1, 2]
    assert table[1][2].noncompile_cpu_ms == pytest.approx(0.002)


# ---- census ----

def _census(path, records):
    path.write_bytes(b"".join(census.RECORD.pack(*r) for r in records))


def test_census_phase_differences_and_report(tmp_path):
    prefix = tmp_path / "census"
    _census(Path(f"{prefix}.pc.a"), [(0x100, 10, 30), (0x200, 1, 1)])
    _census(Path(f"{prefix}.pc.b"), [(0x100, 15, 45), (0x200, 1, 1), (0x300, 4, 40)])
    _census(Path(f"{prefix}.mmio.a"), [(0xFFE02108, 5, 0)])
    _census(Path(f"{prefix}.mmio.b"), [(0xFFE02108, 9, 0), (0xFFC00000, 0, 2)])
    pcs = census.phase(prefix, "pc", "a", "b")
    assert pcs == {0x100: (5, 15), 0x300: (4, 40)}  # unchanged 0x200 dropped
    symbols = tmp_path / "syms.txt"
    symbols.write_text("0x00000100 fir_loop\n0x00000300 main\n")
    text = census.report(prefix, "a", "b", 10, census.Symbols(symbols))
    rows = [line for line in text.splitlines() if line.startswith("0x")]
    assert rows[0].startswith("0x00000300") and rows[0].endswith("main")  # most packets first
    assert "fir_loop" in text and "0xffe02108" in text
    with pytest.raises(ValueError):
        census.phase(prefix, "pc", "b", "a")


def test_perf_preflight_explains_paranoid(monkeypatch):
    class Failed:
        returncode, stderr = 255, "perf_event_open(..., PERF_FLAG_FD_CLOEXEC) failed"

    def with_paranoid(value):
        class FakePath:
            def __init__(self, *_):
                pass

            def read_text(self):
                return f"{value}\n"
        monkeypatch.setattr(perf, "Path", FakePath)

    monkeypatch.setattr(perf.subprocess, "run", lambda *a, **k: Failed())
    with_paranoid(4)
    message = perf.preflight("cpu-clock:u", ["-F", "999"])
    assert "paranoid=2" in message and "restore 4" in message
    with_paranoid(2)
    assert "cycles:upp" in perf.preflight("cycles:pp", ["-c", "400000"])
    assert "u modifier" not in perf.preflight("cpu-clock:u", ["-F", "999"])
    assert "cycles:u" in perf.preflight("cycles", ["-c", "400000"])  # no modifier: kernel too


@pytest.mark.parametrize("rate", [["--period", "0"], ["--freq", "0"], ["--period", "-5"]])
def test_perf_rate_must_be_positive(files, rate):
    with pytest.raises(SystemExit):
        cli.main(["perf", "--binary", str(FAKE), "--nand", str(files / "nand.bin"), "--otp", str(files / "otp.bin"),
                  "--window", "main-boot:main-display", *rate])


def test_lbr_call_graph_needs_a_hardware_event():
    assert "hardware event" in perf.check_call_graph("cpu-clock:u", "lbr")
    assert "hardware event" in perf.check_call_graph("task-clock", "lbr")
    assert perf.is_software_event("cpu-clock") and perf.is_software_event("task-clock:u")
    assert not perf.is_software_event("cycles:u") and not perf.is_software_event("cpu_core/cycles/u")
    assert perf.check_call_graph("cycles:u", "lbr") is None
    assert perf.check_call_graph("cpu-clock:u", "fp") is None


# ---- host counters and interrupts (trace v3) ----

def test_mark_lines_carry_interrupts_process_cpu_and_host_counters():
    m = parse_mark("[mark] a pc=0x0 wall_ms=2.5 cpu_ms=2.0 process_cpu_ms=2.125 runs=3 packets=10 cycles=10 "
                   "translated=1 mmio_r=0 mmio_w=0 events=1 irq=4 host_insns=900 host_cycles=1800 deterministic=1")
    assert m.workload()["irq"] == 4 and m.counters["process_cpu_ms"] == 2.125 and m.counters["host_insns"] == 900


def test_interrupts_gate_the_workload_only_when_both_sides_count_them():
    line = ("[mark] a pc=0x0 wall_ms=0 cpu_ms=0 runs=1 packets=10 cycles=10 translated=1 mmio_r=0 mmio_w=0 "
            "events=0{irq} host_insns={insns}")
    old, new = parse_mark(line.format(irq="", insns=1)), parse_mark(line.format(irq=" irq=5", insns=2))
    assert workload_diff([old], [new]) == []                       # an older binary has no irq
    assert workload_diff([new], [parse_mark(line.format(irq=" irq=6", insns=2))])
    assert workload_diff([new], [parse_mark(line.format(irq=" irq=5", insns=99))]) == []  # host cost
    # A side that reports irq at one mark but not another is not an older binary.
    second = lambda irq: parse_mark(line.format(irq=irq, insns=1).replace("[mark] a ", "[mark] b "))
    assert any("missing" in p for p in workload_diff([new, second(" irq=5")], [new, second("")]))


def test_counters_split_host_instructions_like_cpu(files, tmp_path):
    result = run(spec(files, counters=True), tmp_path / "r")
    assert "--profile-counters" in result.command and result.reached
    phase = trace.phase(trace.load(str(result.trace_path)), "start", "main-frame")
    # Three translations of 5M instructions and one cache flush of 2M are JIT work.
    assert phase.compile_insns == 17_000_000 and phase.noncompile_insns == 90_000_000
    assert phase.host_cycles == 2 * phase.insns
    assert phase.other_threads_cpu_ms == pytest.approx(0.0)
    plain = trace.phase(trace.load(str(run(spec(files), tmp_path / "p").trace_path)), "start", "main-frame")
    assert plain.insns is None and plain.noncompile_insns is None
    with pytest.raises(ValueError):
        spec(files, args=["--profile-counters"])


def test_ab_reports_host_instructions_beside_cpu(files, tmp_path):
    out = tmp_path / "ab"
    out.mkdir()
    fast = tmp_path / "fast.sh"
    fast.write_text(f"#!/bin/sh\nFAKE_SCALE=0.8 exec {FAKE} \"$@\"\n")
    fast.chmod(0o755)
    new = ab.Side("new", snapshot_binary(fast, out), [], sha256_file(fast))
    report = ab.compare(_side("base", files, out), new, spec(files, counters=True), 4, out,
                        ("start", "main-frame"), 2.0, False)
    insns = report["summary"]["noncompile_insns"]
    assert insns["mean_pct"] == pytest.approx(-20.0) and report["status"] == "faster"
    plain = tmp_path / "plain"
    plain.mkdir()
    assert "noncompile_insns" not in ab.compare(_side("base", files, plain), _side("new", files, plain), spec(files),
                                                4, plain, ("start", "main-frame"), 2.0, False)["summary"]


def test_ab_gates_on_interrupts(files, tmp_path):
    out = tmp_path / "ab"
    out.mkdir()
    more = tmp_path / "more.sh"
    more.write_text(f"#!/bin/sh\nFAKE_IRQ=4 exec {FAKE} \"$@\"\n")
    more.chmod(0o755)
    new = ab.Side("new", snapshot_binary(more, out), [], sha256_file(more))
    report = ab.compare(_side("base", files, out), new, spec(files), 4, out, ("start", "main-frame"), 2.0, False)
    assert report["status"].startswith("workload changed")
    assert any(".irq:" in line for p in report["pairs"] for line in p["workload_diff"])


# ---- statistics and machine rigor ----

def test_small_samples_use_the_wider_t_interval_and_report_a_sign_test():
    c = stats.paired([100] * 4, [96, 99, 97, 98])
    assert c.t_ci_pct[0] < c.bootstrap_ci_pct[0] and (c.ci_low_pct, c.ci_high_pct)[0] == c.t_ci_pct[0]
    assert c.bootstrap_ci_pct[0] >= -4.0          # a bootstrap of four cannot pass their minimum
    assert c.sign_p == pytest.approx(0.125)       # four wins out of four
    assert stats.sign_test_p(0, 0) == 1.0 and stats.sign_test_p(5, 5) == 1.0


def test_a_real_but_small_difference_is_not_inconclusive():
    c = stats.paired([100] * 6, [99.0, 99.2, 98.9, 99.1, 99.0, 98.8])
    assert c.ci_high_pct < 0 and c.verdict(2.0) == stats.FASTER_SMALL
    assert stats.paired([100] * 6, [101.0, 100.8, 101.1, 100.9, 101.0, 101.2]).verdict(2.0) == stats.SLOWER_SMALL
    assert stats.paired([100] * 4, [99, 101, 100, 100]).verdict(2.0) == stats.INCONCLUSIVE


def test_spearman_permutation_p():
    assert stats.spearman_p(range(8), range(8)) < 0.001
    assert stats.spearman_p(range(6), [3, 1, 4, 1, 5, 9]) > 0.05


def test_spearman():
    assert stats.spearman([1, 2, 3, 4], [10, 20, 30, 40]) == pytest.approx(1.0)
    assert stats.spearman([1, 2, 3, 4], [4, 3, 2, 1]) == pytest.approx(-1.0)
    assert stats.spearman([1, 2, 3], [5, 5, 5]) == 0.0


@pytest.fixture
def hybrid(tmp_path, monkeypatch):
    sys_root = tmp_path / "sys"
    for pmu, cpus in (("cpu_core", "0-3"), ("cpu_atom", "4-5")):
        (sys_root / "devices" / pmu).mkdir(parents=True)
        (sys_root / "devices" / pmu / "cpus").write_text(cpus + "\n")
    for cpu, siblings in ((2, "2-3"), (3, "2-3"), (4, "4")):
        topo = sys_root / "devices" / "system" / "cpu" / f"cpu{cpu}" / "topology"
        topo.mkdir(parents=True)
        (topo / "thread_siblings_list").write_text(siblings + "\n")
    monkeypatch.setattr(machine, "SYS", str(sys_root))
    # Any CPU of the fake machine, whatever this host's affinity is.
    monkeypatch.setattr(machine.os, "sched_getaffinity", lambda pid: set(range(6)))
    return sys_root


def test_hybrid_cpus_need_a_pinned_performance_core(hybrid):
    assert machine.core_types() == {"performance": {0, 1, 2, 3}, "efficiency": {4, 5}}
    assert machine.siblings(2) == [3] and machine.siblings(4) == []
    assert any("pass --cpu with a performance core (0-3)" in p for p in machine.preflight(None, 0, require_pin=True))
    assert not any("--cpu" in p for p in machine.preflight(None, 0))     # a single run may float
    assert any("efficiency core" in p for p in machine.preflight(4, 0))
    assert not any("core" in p for p in machine.preflight(2, 0, require_pin=True))


def test_an_inherited_single_cpu_affinity_counts_as_pinned(hybrid, monkeypatch):
    monkeypatch.setattr(machine.os, "sched_getaffinity", lambda pid: {2})       # taskset -c 2
    assert not any("core" in p for p in machine.preflight(None, 0, require_pin=True))
    monkeypatch.setattr(machine.os, "sched_getaffinity", lambda pid: {4})       # taskset -c 4
    assert any("CPU 4 is an efficiency core" in p for p in machine.preflight(None, 0))
    assert any("not in this process's affinity mask" in p for p in machine.preflight(2, 0))


def test_sibling_load_during_a_run_comes_from_cpu_times():
    before = {"cpu_times": {"2": [100, 1000], "3": [50, 1000]}}
    after = {"cpu_times": {"2": [1100, 2000], "3": [350, 2000]}}
    assert machine.sibling_busy(before, after, 2) == pytest.approx(0.3)
    assert machine.sibling_busy(before, after, None) is None


def _drifting(tmp_path, name, step):
    """A fake whose execution slows by `step` on every call (a drifting machine)."""
    counter = tmp_path / f"{name}.count"
    script = tmp_path / f"{name}.sh"
    script.write_text(f"#!/bin/sh\nn=$(cat {counter} 2>/dev/null || echo 0); echo $((n + 1)) > {counter}\n"
                      f"FAKE_SCALE=$(python3 -c \"print(1 + $n * {step})\") exec {FAKE} \"$@\"\n")
    script.chmod(0o755)
    return script


def test_extend_adds_pairs_to_a_batch_and_drift_is_reported(files, tmp_path, monkeypatch):
    out = tmp_path / "ab"
    out.mkdir()
    slow = _drifting(tmp_path, "drift", 0.05)
    side = lambda name: ab.Side(name, snapshot_binary(slow, out), [], sha256_file(slow))
    report = ab.compare(side("base"), side("new"), spec(files), 4, out, ("start", "main-frame"), 2.0, False)
    assert len(report["pairs"]) == 4 and report["drift"] == []         # too few runs to call a trend
    assert cli.main(["extend", str(out), "--pairs", "2", "--force"]) == 0
    report = json.loads((out / "results.json").read_text())
    assert len(report["pairs"]) == 6 and report["extensions"] == 1
    assert [p["order"] for p in report["pairs"]][4:] == [["new", "base"], ["base", "new"]]
    assert any("noncompile_cpu_ms trends with run order" in line for line in report["drift"])
    assert report["summary"]["noncompile_cpu_ms"]["pairs"] == 6
    with pytest.raises(SystemExit):
        cli.main(["extend", str(out), "--pairs", "3", "--force"])       # odd: orders would not balance
    (files / "otp.bin").write_text("another image")
    with pytest.raises(SystemExit, match="input otp_sha256 changed"):
        cli.main(["extend", str(out), "--pairs", "2", "--force"])


def test_only_a_finished_batch_can_be_extended(files, tmp_path):
    out = tmp_path / "ab"
    out.mkdir()
    failing = tmp_path / "failing.sh"
    failing.write_text(f"#!/bin/sh\nFAKE_MODE=fail exec {FAKE} \"$@\"\n")
    failing.chmod(0o755)
    new = ab.Side("new", snapshot_binary(failing, out), [], sha256_file(failing))
    report = ab.compare(_side("base", files, out), new, spec(files), 4, out, ("start", "main-frame"), 2.0, False)
    assert report["status"].startswith("failed")
    with pytest.raises(SystemExit, match="only a finished batch"):
        cli.main(["extend", str(out), "--pairs", "2", "--force"])


def test_pairs_with_a_busy_sibling_are_reported(files, tmp_path, monkeypatch):
    monkeypatch.setattr(machine, "sibling_busy", lambda before, after, cpu: 0.5)
    out = tmp_path / "ab"
    out.mkdir()
    report = ab.compare(_side("base", files, out), _side("new", files, out), spec(files), 4, out,
                        ("start", "main-frame"), 2.0, False)
    assert report["contended_pairs"] == 4
    assert all("SMT sibling 50% busy" in p["contended"][0] for p in report["pairs"])
    assert report["decided_on"] == "noncompile_cpu_ms"   # no uncontended pairs to decide on


def test_status_is_decided_on_uncontended_pairs_when_there_are_enough(files, tmp_path, monkeypatch):
    calls = []

    def busy(before, after, cpu):
        calls.append(1)
        return 0.5 if len(calls) in (1, 2) else 0.0   # only pair 0 shared its core
    monkeypatch.setattr(machine, "sibling_busy", busy)
    out = tmp_path / "ab"
    out.mkdir()
    report = ab.compare(_side("base", files, out), _side("new", files, out), spec(files), 6, out,
                        ("start", "main-frame"), 2.0, False)
    assert report["contended_pairs"] == 1 and report["decided_on"] == "noncompile_cpu_ms_uncontended"
    # Pairs 1-5 were calm: 1, 3, 5 ran AB and 2, 4 BA; two of each are kept.
    assert report["summary"]["noncompile_cpu_ms_uncontended"]["uncontended_pairs"] == [1, 2, 3, 4]


def test_leaving_out_contended_pairs_never_unbalances_the_orders(files, tmp_path, monkeypatch):
    # Contention on every AB pair would leave only BA pairs, where an order
    # effect would read as a difference between identical sides.
    calls = []

    def busy(before, after, cpu):
        calls.append(1)
        pair = (len(calls) - 1) // 2      # one reading per run, two runs per pair
        return 0.5 if pair % 2 == 1 else 0.0
    monkeypatch.setattr(machine, "sibling_busy", busy)
    out = tmp_path / "ab"
    out.mkdir()
    report = ab.compare(_side("base", files, out), _side("new", files, out), spec(files), 8, out,
                        ("start", "main-frame"), 2.0, False)
    assert report["contended_pairs"] == 4 and report["decided_on"] == "noncompile_cpu_ms"
    assert ab.balanced([({}, {"pair": 0, "order": ["new", "base"]}), ({}, {"pair": 2, "order": ["new", "base"]})]) == []


def test_an_input_changed_during_a_batch_stops_it(files, tmp_path):
    out = tmp_path / "ab"
    out.mkdir()
    otp = files / "otp.bin"
    swap = tmp_path / "swap.sh"   # the first run replaces the OTP image under the batch
    swap.write_text(f"#!/bin/sh\necho changed > {otp}\nexec {FAKE} \"$@\"\n")
    swap.chmod(0o755)
    report = ab.compare(ab.Side("base", snapshot_binary(swap, out), [], sha256_file(swap)),
                        _side("new", files, out), spec(files), 4, out, ("start", "main-frame"), 2.0, False)
    assert report["status"].startswith("failed: an input changed during the batch")
    assert "otp_sha256" in report["status"]


def test_an_unpinned_run_has_no_per_cpu_readings(monkeypatch):
    monkeypatch.setattr(machine.os, "sched_getaffinity", lambda pid: {0, 1, 2})
    assert machine.measured_cpu(None) is None
    snap = machine.snapshot(None)
    assert snap.throttle_count is None and snap.core_type is None and snap.cpu_times == {}
    monkeypatch.setattr(machine.os, "sched_getaffinity", lambda pid: {2})
    assert machine.measured_cpu(None) == 2



# ---- compile shape ----

def _shaped_trace():
    t = lambda pc, ts, cpu, h, ok=True: {"name": "translate", "ph": "X", "ts": ts, "dur": 1.0,
                                         "args": {"pc": pc, "ok": ok, "cpu_ns": cpu, "ir_insns": 10, "ir_blocks": 1,
                                                  "object_bytes": 40, "object_hash": h}}
    return {"schema": "op1.trace.v3", "dropped": 0, "traceEvents": [
        {"name": "start", "cat": "mark", "ts": 0.0, "args": {"wall_ns": 0, "thread_cpu_ns": 0}},
        t(0x100, 1.0, 2_000_000, "aa"), {"name": "lift", "ph": "X", "ts": 1.0, "dur": 0.25, "args": {"pc": 0x100}},
        {"name": "materialize", "ph": "X", "ts": 1.3, "dur": 0.75, "args": {"pc": 0x100}},
        t(0x200, 3.0, 1_000_000, "bb"),
        {"name": "a", "cat": "mark", "ts": 4.0, "args": {"wall_ns": 4000, "thread_cpu_ns": 4000}},
        t(0x100, 5.0, 3_000_000, "aa"),            # translated again after a flush
        {"name": "b", "cat": "mark", "ts": 9.0, "args": {"wall_ns": 9000, "thread_cpu_ns": 9000}},
    ]}


def test_compile_report_ranks_blocks_and_counts_retranslations():
    from op1prof import compilation
    data = _shaped_trace()
    spans = compilation.translations(data)
    assert len(spans) == 3 and len(compilation.translations(data, ("a", "b"))) == 1
    shape = compilation.codegen(spans)
    assert shape["distinct_code"] == 2 and shape["translations"] == 3 and shape["ir_insns"] == 30
    text = compilation.report(data, None, 10, census.Symbols(None))
    assert "3 translations of 2 blocks" in text and "translated again: 1 blocks, 1 extra translations, 0.003 s" in text
    assert "lift 25%, materialize 75%" in text
    assert text.index("0x00000100") < text.index("0x00000200")     # ranked by compile CPU
    # The digest ignores host timing and order, only what was compiled.
    other = _shaped_trace()
    other["traceEvents"][1]["args"]["cpu_ns"] = 9
    assert compilation.codegen(compilation.translations(other))["digest"] == shape["digest"]
    other["traceEvents"][4]["args"]["object_hash"] = "cc"
    assert compilation.codegen(compilation.translations(other))["digest"] != shape["digest"]


def test_unknown_hashes_and_failed_translations_give_no_fingerprint():
    from op1prof import compilation
    spans = lambda *hashes: [compilation.Translation(pc=i, begin_us=i, cpu_ns=1, ok=h != "failed", ir_insns=1,
                                                     object_bytes=1, object_hash=None if h == "failed" else h)
                             for i, h in enumerate(hashes)]
    assert compilation.has_shape(spans("aa", "bb"))
    assert not compilation.has_shape(spans("0000000000000000", "0000000000000000"))   # all unknown
    assert not compilation.has_shape(spans("aa", "0000000000000000"))                 # partly unknown
    assert not compilation.has_shape(spans("failed", "failed"))                       # nothing compiled
    assert compilation.has_shape(spans("aa", "failed"))                               # failures carry no code
    assert not compilation.has_shape([])


def test_ab_says_when_both_sides_compiled_identical_code(files, tmp_path, capsys):
    def wrapper(name, env):
        script = tmp_path / f"{name}.sh"
        script.write_text(f"#!/bin/sh\n{env} exec {FAKE} \"$@\"\n")
        script.chmod(0o755)
        return script
    same, other, old = wrapper("same", "FAKE_SCALE=0.99"), wrapper("other", "FAKE_CODE=5"), wrapper("old", "FAKE_NOSHAPE=1")
    unknown = wrapper("unknown", "FAKE_UNKNOWN_HASH=1")
    for name, new, identical in (("same", same, True), ("other", other, False)):
        out = tmp_path / name
        out.mkdir()
        report = ab.compare(_side("base", files, out), ab.Side("new", snapshot_binary(new, out), [], sha256_file(new)),
                            spec(files), 4, out, ("start", "main-frame"), 2.0, False)
        assert report["codegen"]["identical"] is identical and report["codegen"]["repeatable"]
        cli._print_ab(report, out)
        printed = capsys.readouterr().out
        assert ("compile fingerprints are identical" in printed) is identical
        assert "host side" not in printed
    out = tmp_path / "old"
    out.mkdir()
    report = ab.compare(_side("base", files, out), ab.Side("new", snapshot_binary(old, out), [], sha256_file(old)),
                        spec(files), 4, out, ("start", "main-frame"), 2.0, False)
    assert report["codegen"] is None   # a binary without compile shapes: nothing to compare
    for name, side in (("unknown-new", unknown),):
        out = tmp_path / name
        out.mkdir()
        both = ab.Side("base", snapshot_binary(unknown, out), [], sha256_file(unknown))
        report = ab.compare(both, ab.Side("new", snapshot_binary(side, out), ["--x"], sha256_file(side)),
                            spec(files), 4, out, ("start", "main-frame"), 2.0, False)
        assert report["codegen"] is None   # unknown hashes on both sides: no fingerprint, not "identical"


# ---- edges and divergence ----

def test_edge_census_phase_and_report(tmp_path):
    prefix = tmp_path / "census"
    write = lambda mark, records: Path(f"{prefix}.edge.{mark}").write_bytes(
        b"".join(census.EDGE.pack(*r) for r in records))
    write("a", [(0x100, 0x200, 10)])
    write("b", [(0x100, 0x200, 109), (0x100, 0x300, 1), (0x200, 0x100, 100)])
    _census(Path(f"{prefix}.pc.a"), [(0x100, 10, 10)])
    _census(Path(f"{prefix}.pc.b"), [(0x100, 110, 110), (0x200, 100, 100)])
    assert census.edge_phase(prefix, "a", "b") == {(0x100, 0x200): 99, (0x100, 0x300): 1, (0x200, 0x100): 100}
    text = census.edge_report(prefix, "a", "b", 10, census.Symbols(None))
    assert "200 transitions between 2 blocks" in text
    assert "0x00000100          100     2 0x00000200  99.0%" in text
    with pytest.raises(ValueError):
        census.edge_phase(prefix, "b", "a")


def test_diverge_finds_the_first_differing_record(tmp_path):
    from op1prof import diverge
    rec = lambda runs, regs, pc=0x10: diverge.Record(runs, runs, runs, pc, 0, 0, 0, regs, 1, 2)
    same = [rec(0, 5), rec(100, 6), rec(200, 7)]
    assert diverge.first_difference(same, list(same)) is None
    found = diverge.first_difference(same, [rec(0, 5), rec(100, 6), rec(200, 8, pc=0x20)])
    assert found["index"] == 2 and found["fields"] == ["pc", "data_regs"] and found["after"].runs == 100
    assert diverge.first_difference(same, same[:2])["fields"] == ["length"]
    a, b = tmp_path / "a.bin", tmp_path / "b.bin"
    a.write_bytes(b"".join(diverge.RECORD.pack(*vars(r).values()) for r in same))
    b.write_bytes(b"".join(diverge.RECORD.pack(*vars(r).values()) for r in [rec(0, 5), rec(100, 9), rec(200, 9)]))
    text = diverge.report(a, b, census.Symbols(None))
    assert "first difference at checksum 1 (run 100): data_regs" in text and "equal at run 0" in text
    assert "--checksum-from 0" in text


def test_verify_with_checksums_says_where_runs_diverge(files, tmp_path):
    flaky = tmp_path / "flaky.sh"   # the second run's guest state differs from record 2 on
    flaky.write_text(f"#!/bin/sh\nn=$(cat {tmp_path}/n 2>/dev/null || echo 0); echo $((n + 1)) > {tmp_path}/n\n"
                     f"if [ $n = 1 ]; then export FAKE_DIVERGE=2 FAKE_WORK=1003; fi\nexec {FAKE} \"$@\"\n")
    flaky.chmod(0o755)
    report = verify.verify(spec(files, binary=flaky, checksum_every=100), 2, tmp_path / "v", None, None)
    assert report["status"] == "differences found"
    assert "first difference at checksum 2" in report["divergence"]["1"]
    from op1prof import diverge
    assert diverge.RECORD.size == 64 and (tmp_path / "v" / "run00" / "checksum.bin").stat().st_size % 64 == 0


def test_verify_with_checksums_catches_a_state_difference_with_the_same_workload(files, tmp_path):
    flaky = tmp_path / "state.sh"   # same counted work; only the second run's registers differ
    flaky.write_text(f"#!/bin/sh\nn=$(cat {tmp_path}/n 2>/dev/null || echo 0); echo $((n + 1)) > {tmp_path}/n\n"
                     f"if [ $n = 1 ]; then export FAKE_DIVERGE=2; fi\nexec {FAKE} \"$@\"\n")
    flaky.chmod(0o755)
    report = verify.verify(spec(files, binary=flaky, checksum_every=100), 2, tmp_path / "v", None, None)
    assert report["status"] == "differences found"
    assert report["differences"] == ["run 1: guest state differs from checksum 2 (data_regs)"]
    plain = verify.verify(spec(files, checksum_every=100), 2, tmp_path / "w", None, None)
    assert plain["status"] == "deterministic" and "divergence" not in plain


def test_diverge_streams_and_stops_at_the_first_difference():
    from op1prof import diverge
    rec = lambda runs, regs: diverge.Record(runs, runs, runs, 0, 0, 0, 0, regs, 1, 2)
    consumed = []

    def endless(first):
        n = 0
        while True:
            consumed.append(n)
            yield rec(n, first if n == 0 else 0)
            n += 1
    found = diverge.first_difference(endless(1), endless(2))
    assert found["index"] == 0 and len(consumed) == 2   # read one record of each, no more
    found = diverge.first_difference([rec(0, 0)], [rec(0, 0), rec(1, 0)])
    assert found["fields"] == ["length"] and found["ended"] == "a"


def test_edges_must_match_block_runs(tmp_path):
    prefix = tmp_path / "census"
    for mark, edges, pcs in (("a", [], []), ("b", [(0x100, 0x200, 3), (0x200, 0x100, 2)], [(0x100, 3, 9), (0x200, 2, 4)])):
        Path(f"{prefix}.edge.{mark}").write_bytes(b"".join(census.EDGE.pack(*r) for r in edges))
        _census(Path(f"{prefix}.pc.{mark}"), pcs)
    assert "checked against the PC census" in census.edge_report(prefix, "a", "b", 5, census.Symbols(None))
    # Edges counted one block late: the transition into the end mark's block missing.
    Path(f"{prefix}.edge.b").write_bytes(b"".join(census.EDGE.pack(*r) for r in [(0x100, 0x200, 3), (0x200, 0x100, 1)]))
    with pytest.raises(ValueError, match="0x00000200: 1 transitions out, 2 runs"):
        census.edge_report(prefix, "a", "b", 5, census.Symbols(None))


def test_a_run_that_floods_its_output_is_stopped(files, tmp_path, monkeypatch):
    from op1prof import runner
    monkeypatch.setattr(runner, "MAX_LOG_BYTES", 1 << 20)
    result = run(spec(files, env={"FAKE_MODE": "spew"}), tmp_path / "r")
    assert not result.reached and "output passed 1 MiB" in result.error
    assert (tmp_path / "r" / "log.txt").stat().st_size < 4 << 20


# ---- probes, threads and first MMR access ----

def _probe_trace():
    trace_data = _probe_events()
    trace_data["probes"] = {"switch": {"hits": 2, "dropped": 0}, "switch-irq": {"hits": 1, "dropped": 0}}
    return trace_data


def _probe_events():
    hit = lambda name, cycles, current: {"name": name, "cat": "probe", "ph": "i", "ts": cycles / 400.0,
                                         "args": {"pc": 1, "runs": cycles, "packets": cycles, "cycles": cycles,
                                                  "R1": current, "current": current, "prio": None}}
    mark = lambda name, cycles: {"name": name, "cat": "mark", "ph": "i", "ts": cycles / 400.0,
                                 "args": {"cycles": cycles, "wall_ns": cycles, "thread_cpu_ns": cycles}}
    return {"schema": "op1.trace.v3", "dropped": 0, "traceEvents": [
        mark("start", 0), hit("switch", 100, 0xA), hit("switch-irq", 300, 0xB), mark("a", 400),
        hit("switch", 600, 0xA), mark("b", 1000),
        {"name": "table", "cat": "read", "ph": "i", "ts": 2.5, "args": {"mark": "a", "addr": 16, "words": [1, None]}},
    ]}


def test_thread_time_splits_guest_time_between_switches():
    from op1prof import probes
    switches = [(100, 0xA), (300, 0xB), (600, 0xA)]
    assert probes.thread_time(switches, 0, 400) == {-1: 100, 0xA: 200, 0xB: 100}
    assert probes.thread_time(switches, 400, 1000) == {0xB: 200, 0xA: 400}
    assert probes.thread_time(switches, 650, 700) == {0xA: 50}


def test_threads_report_and_perfetto(tmp_path):
    from op1prof import probes
    data = _probe_trace()
    text = probes.threads_report(data, ["switch", "switch-irq"], "current", {0xA: "main"})
    assert "3 switches" in text and "a -> b" in text
    assert "main" in text and "0xb" in text and "(before the first switch)" in text
    with pytest.raises(ValueError, match="no captured value 'R7'"):
        probes.timeline(data, ["switch"], "R7")
    with pytest.raises(ValueError, match="not declared in this trace: swich"):
        probes.timeline(data, ["switch", "swich"], "current")   # a typo must not drop switches
    data["probes"]["quiet"] = {"hits": 0, "dropped": 0}
    with pytest.raises(ValueError, match="no hits of quiet"):
        probes.timeline(data, ["quiet"], "current")
    del data["probes"]["quiet"]
    out = tmp_path / "threads.json"
    probes.perfetto(data, ["switch", "switch-irq"], "current", {0xA: "main"}, out)
    events = json.loads(out.read_text())["traceEvents"]
    slices = [e for e in events if e["ph"] == "X"]
    assert [(e["name"], e["ts"], e["dur"]) for e in slices] == [("main", 0.25, 0.5), ("0xb", 0.75, 0.75),
                                                                ("main", 1.5, 1.0)]
    data["probes"] = {"switch": {"hits": 70000, "dropped": 4464}, "switch-irq": {"hits": 1, "dropped": 0}}
    with pytest.raises(ValueError, match="dropped hits.*switch 4464"):
        probes.timeline(data, ["switch", "switch-irq"], "current")
    assert "4464 more dropped" in probes.summary(data, 3)
    data["probes"] = {"switch": {"hits": 2, "dropped": 0}, "switch-irq": {"hits": 1, "dropped": 0}}
    summary = probes.summary(data, 3)
    assert "switch: 2 hits" in summary and "prio         - x2" in summary
    assert "read table at a (0x00000010): 1,-" in summary


def test_first_mmr_access_report(tmp_path):
    prefix = tmp_path / "census"
    _census(Path(f"{prefix}.mmio.a"), [(0xffc00800, 1, 0)])
    _census(Path(f"{prefix}.mmio.b"), [(0xffc00800, 3, 1), (0xffc00c40, 0, 2)])
    Path(f"{prefix}.mmiofirst.b").write_bytes(census.FIRST.pack(0xffc00800, 0x100, 400, 900) +
                                              census.FIRST.pack(0xffc00c40, 0x200, 4_000_000, 4_000_000))
    rows = census.mmio_first(prefix, "a", "b", (1000, 5_000_000))
    assert [r["addr"] for r in rows] == [0xffc00c40] and rows[0]["writes"] == 2
    text = census.mmio_first_report(rows, "a", "b", 10, census.Symbols(None))
    assert "first accessed a -> b: 1" in text           # 0xffc00800 was first touched before the phase
    assert "0xffc00c40     10.000          0          2 0x00000200" in text


def test_run_passes_probes(files, tmp_path):
    result = run(spec(files, probes=True), tmp_path / "r")
    assert "--profile-probes" in result.command and result.reached


def test_run_dumps_ir_of_chosen_blocks_and_annotate_needs_a_precise_event(files, tmp_path, monkeypatch):
    result = run(spec(files, dump_pcs=[0x1a5e27e, 0xffa00000]), tmp_path / "r")
    at = result.command.index("--profile-dump-ir")
    assert result.command[at + 1:at + 4] == ["1a5e27e,ffa00000", "--profile-dump-dir", "ir"]
    capture = tmp_path / "capture"
    capture.mkdir()
    (capture / "capture.json").write_text(json.dumps({"event": "cpu-clock:u", "precise": False, "window": ["a", "b"]}))
    with pytest.raises(ValueError, match="not a precise event"):
        perf.annotate(capture, 0x1a5e27e, False, 10)
    calls = []

    class Done:
        returncode, stdout, stderr = 0, "\n".join(f"line {i}" for i in range(5)), ""
    monkeypatch.setattr(perf.subprocess, "run", lambda command, **kw: calls.append(command) or Done())
    text = perf.annotate(capture, 0x1a5e27e, True, 3)
    assert calls[0][-2:] == ["-s", "bb_0x01a5e27e"]
    assert "shares are where the timer landed" in text and "... 2 more lines" in text


# ---- gate and run hygiene ----

def test_events_are_shape_and_latched_interrupts_are_work():
    line = ("[mark] a pc=0x0 wall_ms=0 cpu_ms=0 runs=1 packets=10 cycles=10 translated=1 mmio_r=0 mmio_w=0 "
            "events={events} irq=5{latched}")
    base = parse_mark(line.format(events=40, latched=" irq_latched=5"))
    # Fewer repeated raises of an asserted level change nothing the guest sees.
    assert workload_diff([base], [parse_mark(line.format(events=12, latched=" irq_latched=5"))]) == []
    assert workload_diff([base], [parse_mark(line.format(events=40, latched=" irq_latched=4"))])
    # A binary older than irq_latched is compared on the rest.
    assert workload_diff([base], [parse_mark(line.format(events=40, latched=""))]) == []


@pytest.mark.parametrize("flag", ["--profile-census", "--profile-probes", "--profile-checksum", "--perf-jitdump",
                                  "--profile-dump-ir"])
@pytest.mark.parametrize("where", ["--arg", "--base-arg", "--new-arg"])
def test_ab_refuses_diagnostic_instrumentation(files, tmp_path, flag, where):
    with pytest.raises(SystemExit, match="refuses diagnostic instrumentation"):
        cli.main(["ab", "--base-bin", str(FAKE), "--nand", str(files / "nand.bin"), "--otp", str(files / "otp.bin"),
                  "--marks", str(files / "marks.json"), "--force", "--out", str(tmp_path / "a"),
                  f"{where}={flag}", "--new-arg=--rtc-epoch"])
    assert not (tmp_path / "a").exists()


@pytest.mark.parametrize("command,extra", [
    ("run", ["--phase", "bootrom:nowhere"]),
    ("run", ["--phase", "main-display:bootrom"]),
    ("run", ["--phase", "bootrom"]),
    ("run", ["--until", "nowhere"]),
    ("run", ["--until", "bootrom", "--phase", "start:main-display"]),
    ("verify", ["--phase", "start:bootrom"]),
    ("verify", ["--until", "nowhere"]),
])
def test_phase_and_until_are_checked_before_any_run(files, tmp_path, command, extra):
    with pytest.raises(SystemExit):
        _cli(files, command, "--out", str(tmp_path / "r"), *extra)
    assert not (tmp_path / "r").exists()


def test_ab_checks_its_phase_before_any_run(files, tmp_path):
    with pytest.raises(SystemExit, match="unknown mark"):
        cli.main(["ab", "--base-bin", str(FAKE), "--nand", str(files / "nand.bin"), "--otp", str(files / "otp.bin"),
                  "--marks", str(files / "marks.json"), "--force", "--out", str(tmp_path / "a"), "--aa",
                  "--phase", "start:lap-end"])
    assert not (tmp_path / "a").exists()


def test_files_read_back_must_have_their_schema(files, tmp_path):
    expect = tmp_path / "expect.json"
    expect.write_text(json.dumps({"marks": []}))
    with pytest.raises(SystemExit, match="not an op1prof expect file"):
        _cli(files, "verify", "--out", str(tmp_path / "v"), "--expect", str(expect))
    assert not (tmp_path / "v").exists()
    batch = tmp_path / "batch"
    batch.mkdir()
    (batch / "results.json").write_text(json.dumps({"schema": "op1prof.ab.v1", "status": "inconclusive",
                                                    "config": {}}))
    with pytest.raises(SystemExit):
        cli.main(["extend", str(batch), "--pairs", "2", "--force"])


def test_a_trace_without_deterministic_time_is_refused(tmp_path):
    path = tmp_path / "t.json"
    path.write_text(json.dumps({**_trace([]), "deterministic": False}))
    with pytest.raises(trace.TraceError, match="without --deterministic"):
        trace.load(str(path))
    path.write_text(json.dumps({**_trace([]), "deterministic": True}))
    assert trace.load(str(path))


def test_mmio_first_names_marks_that_did_not_fire(files, tmp_path, capsys):
    assert _cli(files, "run", "--out", str(tmp_path / "r"), "--until", "main-display") == 0
    assert cli.main(["census", str(tmp_path / "r" / "run"), "--phase", "bootrom:main-frame", "--mmio-first"]) == 2
    assert "did not fire" in capsys.readouterr().err


def _batch(files, out):
    """A finished A/A batch of four pairs in `out`, and its saved report."""
    out.mkdir()
    ab.compare(_side("base", files, out), _side("new", files, out), spec(files), 4, out,
               ("start", "main-frame"), 2.0, False)
    return json.loads((out / "results.json").read_text())


def test_extend_holds_saved_settings_to_the_instrumentation_rule(files, tmp_path):
    # A batch an older op1prof started with one side's probes on.
    out = tmp_path / "ab"
    saved = _batch(files, out)
    saved["config"]["sides"]["new"]["args"] = ["--profile-probes"]
    (out / "results.json").write_text(json.dumps(saved))
    with pytest.raises(SystemExit, match="refuses diagnostic instrumentation"):
        cli.main(["extend", str(out), "--pairs", "2", "--force"])
    assert len(json.loads((out / "results.json").read_text())["pairs"]) == 4   # nothing ran


def test_extend_judges_earlier_pairs_by_the_current_gate(files, tmp_path):
    # An older op1prof gated on events and recorded these differences.
    out = tmp_path / "ab"
    saved = _batch(files, out)
    for pair in saved["pairs"]:
        pair["workload_diff"] = ["main-display.events: 6 vs 4 (-33.333%)"]
    saved["status"] = ab.WORKLOAD_CHANGED
    (out / "results.json").write_text(json.dumps(saved))
    cli.main(["extend", str(out), "--pairs", "2", "--force"])
    report = json.loads((out / "results.json").read_text())
    assert [p["workload_diff"] for p in report["pairs"]] == [[]] * 6
    assert report["status"] != ab.WORKLOAD_CHANGED and not report["workload_changed"]


# ---- build identity, cache flushes, instrumentation ----

def test_cache_flushes_are_jit_time_not_execution():
    t = _trace([_mark("a", 0, 0, 0), {**_translate(10, 20, 15000), "args": {"cpu_ns": 15000, "pc": 0x100}},
                {"name": "invalidate", "cat": "jit", "ph": "X", "ts": 40, "dur": 50, "args": {"cpu_ns": 45000}},
                _mark("b", 1000, 1_000_000, 900_000)])
    p = trace.phase(t, "a", "b")
    assert p.translations == 1 and p.invalidations == 1
    assert p.compile_cpu_ms == pytest.approx(0.060) and p.invalidate_cpu_ms == pytest.approx(0.045)
    assert p.noncompile_cpu_ms == pytest.approx(0.840)
    assert "cache flushes: 1, 0.04 ms CPU" in compilation.report(t, ("a", "b"), 5, census.Symbols(None))


def test_runs_record_the_build_and_ab_warns_about_different_kinds(files, tmp_path, monkeypatch, capsys):
    result = run(spec(files, trace_events=1000), tmp_path / "r")
    assert result.build["schema"] == "op1.build.v1" and result.to_json()["build"] == result.build
    assert result.command[result.command.index("--profile-trace-events") + 1] == "1000"
    debug = tmp_path / "debug.sh"
    debug.write_text(f"#!/bin/sh\nFAKE_BUILD='{{\"build_type\": \"Debug\", \"host\": \"fedcba987654\"}}' "
                     f"exec {FAKE} \"$@\"\n")
    debug.chmod(0o755)
    assert cli.main(["ab", "--base-bin", str(FAKE), "--new-bin", str(debug), "--nand", str(files / "nand.bin"),
                     "--otp", str(files / "otp.bin"), "--marks", str(files / "marks.json"), "--force",
                     "--out", str(tmp_path / "ab"), "--timeout", "30", "--progress-timeout", "5"]) in (0, 1)
    out = capsys.readouterr()
    assert "builds: base 0123456789ab bcore ba9876543210; new fedcba987654 bcore ba9876543210" in out.out
    assert "different kinds of build (build_type Release vs Debug)" in out.err
    report = json.loads((tmp_path / "ab" / "results.json").read_text())
    assert report["builds"]["kind_differs"] == ["build_type"]


def test_an_older_binary_without_a_build_line_still_runs(files, tmp_path, monkeypatch):
    monkeypatch.setenv("FAKE_BUILD", "none")
    result = run(spec(files), tmp_path / "r")
    assert result.reached and result.build is None
    assert "--profile-trace-events" in RESERVED_ARGS


def test_verify_instrumentation_names_the_setting_that_changes_the_workload(files, tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(verify, "_counters_unavailable", lambda: None)
    assert _cli(files, "verify", "--instrumentation", "--out", str(tmp_path / "same")) == 0
    report = json.loads((tmp_path / "same" / "results.json").read_text())
    assert [c["name"] for c in report["configurations"]] == ["plain", "census", "probes", "checksum", "counters",
                                                             "no trace"]
    assert report["configurations"][2]["skipped"] == "the marks file declares no probes"
    assert len(report["runs"]) == 5 and report["status"] == "deterministic"
    commands = [r["command"] for r in report["runs"]]
    assert "--profile-census" in commands[1] and "--profile-trace" not in commands[-1]
    capsys.readouterr()
    monkeypatch.setenv("FAKE_SKEW", "--profile-census")
    assert _cli(files, "verify", "--instrumentation", "--out", str(tmp_path / "skew")) == 1
    out = capsys.readouterr().out
    assert "census    DIFFERENT\n  bootrom.packets: 1000 vs 1001" in out and "checksum  same workload" in out
    assert "differences found" in out
    with pytest.raises(SystemExit, match="takes no --runs"):
        _cli(files, "verify", "--instrumentation", "--runs", "3", "--out", str(tmp_path / "x"))


def test_verify_instrumentation_refuses_instrumentation_for_every_configuration(files, tmp_path):
    # --arg reaches every configuration, the plain one included: a setting
    # enabled there would be compared with itself.
    with pytest.raises(SystemExit, match="switches each setting on by itself"):
        _cli(files, "verify", "--instrumentation", "--arg=--profile-probes", "--out", str(tmp_path / "v"))
    assert not (tmp_path / "v").exists()


@pytest.mark.parametrize("command", ["run", "verify"])
def test_trace_events_reach_the_emulator(files, tmp_path, command):
    assert _cli(files, command, "--trace-events", "500000", "--out", str(tmp_path / "o")) == 0
    report = json.loads((tmp_path / "o" / "results.json").read_text())
    runs = [report["run"]] if command == "run" else report["runs"]
    for entry in runs:
        assert entry["command"][entry["command"].index("--profile-trace-events") + 1] == "500000"
    with pytest.raises(SystemExit):
        _cli(files, command, "--trace-events", "0", "--out", str(tmp_path / "z"))


# ---- service census ----

def _service_dump(path, scale, dropped=0):
    """A cumulative service dump: two RTCs-worth of polling, a DMA that moves data."""
    def source(name, type_, device, calls, productive, published, transitions, queued=0, raises=0, work=0):
        return {"name": name, "type": type_, "device": device, "calls": calls, "productive": productive,
                "published": published, "transitions": transitions, "queued": queued, "raises": raises,
                "work": work}
    data = {"schema": "op1.service.v1",
            "sources": [source("RTC", "RTC", True, 1000 * scale, 2 * scale, 1000 * scale, 2 * scale),
                        source("DMA", "DMA", True, 1000 * scale, 10 * scale, 0, 0, work=4096 * scale),
                        source("TWI", "TWI", True, 1000 * scale, 0, 0, 0),
                        source("block", "", False, 0, 0, 30 * scale, 3 * scale, queued=5 * scale),
                        source("events", "", False, 0, 0, 0, 0),
                        source("other", "", False, 0, 0, 0, 0, raises=900 * scale)],
            "pins": [{"pin": 26, "published": 1000 * scale, "transitions": 2 * scale, "source": "RTC"}],
            "ivg": [{"level": 9, "forwarded": 900 * scale, "raised": 900 * scale, "latched": 3 * scale,
                     "entered": 3 * scale}] + [{"level": lvl, "forwarded": 0, "raised": 0, "latched": 0, "entered": 0}
                                              for lvl in range(10, 16)],
            "queue": {"process_calls": 1000 * scale, "nonempty_calls": 900 * scale, "length_sum": 950 * scale,
                      "length_max": 4, "queued_other_threads": 2},
            "timer_ticks_dropped": dropped,
            "bcore": {"cache_hits": 990 * scale, "cache_misses": 10 * scale, "blocks_executed": 1000 * scale}}
    path.write_text(json.dumps(data))


def test_service_phase_differences_and_report(tmp_path):
    prefix = tmp_path / "census"
    _service_dump(Path(f"{prefix}.service.a"), 1)
    _service_dump(Path(f"{prefix}.service.b"), 3, dropped=5)
    data = service.phase(prefix, "a", "b")
    rtc = data["sources"][0]
    assert rtc["calls"] == 2000 and rtc["productive"] == 4 and rtc["transitions"] == 4
    assert data["pins"] == [{"pin": 26, "source": "RTC", "published": 2000, "transitions": 4}]
    assert [r["level"] for r in data["ivg"]] == [9] and data["ivg"][0]["raised"] == 1800
    assert data["queue"]["process_calls"] == 2000 and data["queue"]["queued_other_threads"] == 0
    text = service.report(data, 10)
    assert "RTC                RTC                   2,000          4    0.20%" in text
    assert "and 1 devices called 2,000 times or fewer that never did anything: TWI" in text
    assert "\nTWI " not in text
    assert "   26 RTC                       2,000           4    0.20%" in text
    assert "0.950 events waiting on average" in text and "warning: 5 GP timer ticks were dropped" in text
    with pytest.raises(ValueError, match="went down"):
        service.phase(prefix, "b", "a")
    with pytest.raises(ValueError, match="no service census"):
        service.phase(prefix, "a", "zz")


def test_service_join_prices_calls_and_refuses_other_captures():
    def data():
        return {"phase": ["a", "b"], "sources": [
            {"name": "PORTF", "type": "GPIO", "device": True, "calls": 1000, "productive": 0},
            {"name": "PORTG", "type": "GPIO", "device": True, "calls": 1000, "productive": 10},
            {"name": "block", "type": "", "device": False, "calls": 0, "productive": 0}]}
    capture = {"window": ["a", "b"], "inclusive_valid": True, "event": "cycles:u",
               "host_inclusive": [{"symbol": "GPIO::ProcessWithInterrupt(int)", "seconds": 0.002}]}
    joined = service.join(data(), capture, {"GPIO"})
    portg = joined["sources"][1]
    assert portg["seconds"] == pytest.approx(0.001) and portg["ns_per_call"] == pytest.approx(1000.0)
    assert portg["ns_per_productive"] == pytest.approx(100_000.0) and joined["sources"][0]["ns_per_productive"] is None
    with pytest.raises(ValueError, match="window"):
        service.join(data(), {**capture, "window": ["a", "c"]}, {"GPIO"})
    with pytest.raises(ValueError, match="lbr"):
        service.join(data(), {**capture, "inclusive_valid": False}, {"GPIO"})


def test_service_capture_must_be_the_same_run(tmp_path, capsys):
    # service --capture applies the census join's check: here the capture
    # ran half the blocks, so its time per call would be halved.
    marks = [_mark_entry("a", 10), _mark_entry("b", 20)]
    capture = _capture(tmp_path, _perf_trace(), [marks[0], {**marks[1], "runs": 2}])
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    _census_run_entry(run_dir, marks)
    _service_dump(run_dir / "census.service.a", 1)
    _service_dump(run_dir / "census.service.b", 2)
    assert cli.main(["service", str(run_dir), "--phase", "a:b", "--capture", str(capture)]) == 2
    assert "another number of blocks" in capsys.readouterr().err


def test_every_run_records_its_binary_beside_its_files(files, tmp_path):
    result = run(spec(files), tmp_path / "r")
    saved = json.loads((tmp_path / "r" / "run.json").read_text())
    assert saved["binary_sha256"] == sha256_file(FAKE) == result.binary_sha256
    assert saved["marks"] == result.to_json()["marks"]


def test_service_command(tmp_path, capsys):
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    _service_dump(run_dir / "census.service.a", 1)
    _service_dump(run_dir / "census.service.b", 2)
    out = tmp_path / "service.json"
    assert cli.main(["service", str(run_dir), "--phase", "a:b", "--json", str(out)]) == 0
    assert "event queue: 1,000 ProcessEvents calls" in capsys.readouterr().out
    assert json.loads(out.read_text())["schema"] == "op1prof.service.v1"
    assert cli.main(["service", str(run_dir), "--phase", "a:missing"]) == 2


def test_the_same_run_also_means_the_same_guest_settings(files, tmp_path):
    # The same work counted under another RTC epoch: the guest read other
    # RTC values, so the census does not describe the captured run.
    capture = tmp_path / "capture"
    capture.mkdir()
    run(spec(files, args=["--rtc-epoch", "100"]), capture / "run")
    other = run(spec(files, args=["--rtc-epoch", "200"]), tmp_path / "other")
    with pytest.raises(ValueError, match="another guest_args"):
        perf_report.check_same_run(capture, other.directory)
    # Diagnostic options of the census run are expected and not compared.
    census_run = run(spec(files, args=["--rtc-epoch", "100", "--profile-probes", "--profile-checksum-every", "9"]),
                     tmp_path / "census")
    assert census_run.guest_args == ["--rtc-epoch", "100"]
    perf_report.check_same_run(capture, census_run.directory)
    # perf's own options (added by op1prof perf) are diagnostic too.
    from op1prof.runner import guest_args
    assert guest_args(["--perf-ctl-fifo", "/x/ctl", "--perf-window", "a:b", "--perf-jitdump"]) == []
    script = run(spec(files, args=["--rtc-epoch", "100"], script="volume 7\nwait 86400000\n"), tmp_path / "script")
    with pytest.raises(ValueError, match="another script"):
        perf_report.check_same_run(capture, script.directory)


def test_a_file_argument_is_compared_by_content(files, tmp_path):
    # The same LDR path, rebuilt between the capture and the census.
    ldr = tmp_path / "tiny.ldr"
    ldr.write_bytes(b"first")
    capture = tmp_path / "capture"
    capture.mkdir()
    first = run(spec(files, args=[str(ldr)]), capture / "run")
    assert first.guest_args == [f"{ldr} sha256:{sha256_file(ldr)}"]
    same = run(spec(files, args=[str(ldr)]), tmp_path / "same")
    perf_report.check_same_run(capture, same.directory)
    ldr.write_bytes(b"second")
    rebuilt = run(spec(files, args=[str(ldr)]), tmp_path / "rebuilt")
    with pytest.raises(ValueError, match="another guest_args"):
        perf_report.check_same_run(capture, rebuilt.directory)


def test_devices_that_inherit_their_service_method_share_its_time():
    # As in op1emu: GPIO and SPORT run Device::ProcessWithInterrupt (a weak
    # symbol), RTC its own; a class without the method anywhere is unpriced.
    implemented = service.parse_implementing([
        "00000000003e0df0 W Device::ProcessWithInterrupt(int)",
        "0000000000414970 T RTC::ProcessWithInterrupt(int)",
        "0000000000414000 T RTC::UpdateInterrupts()"])
    assert implemented == {"Device", "RTC"}
    def source(name, type_, bases, calls):
        return {"name": name, "type": type_, "bases": bases, "device": True, "calls": calls, "productive": 0,
                "published": 1, "transitions": 0, "queued": 0, "raises": 0, "work": 0}
    data = {"phase": ["a", "b"], "sources": [
        source("PORTF", "GPIO", ["RegisterDevice", "Device", "GPIOPeripheral"], 1000),
        source("SPORT0", "SPORT", ["RegisterDevice", "Device", "DMABus"], 3000),
        source("RTC", "RTC", ["RegisterDevice", "Device"], 1000),
        source("Odd", "Odd", [], 1000)], "pins": [], "ivg": [],
        "queue": {"process_calls": 1, "nonempty_calls": 0, "length_sum": 0, "length_max_so_far": 0,
                  "queued_other_threads": 0}, "timer_ticks_dropped": 0,
        "bcore": {"blocks_executed": 1, "cache_hits": 1, "cache_misses": 0}}
    capture = {"window": ["a", "b"], "inclusive_valid": True, "event": "cycles:u", "host_inclusive": [
        {"symbol": "Device::ProcessWithInterrupt(int)", "seconds": 0.020},
        {"symbol": "RTC::ProcessWithInterrupt(int)", "seconds": 0.005}]}
    joined = {s["name"]: s for s in service.join(data, capture, implemented)["sources"]}
    assert joined["PORTF"]["implementation"] == "Device" and joined["PORTF"]["seconds"] == pytest.approx(0.005)
    assert joined["SPORT0"]["seconds"] == pytest.approx(0.015) and joined["SPORT0"]["ns_per_call"] == pytest.approx(5000)
    assert joined["RTC"]["seconds"] == pytest.approx(0.005)
    assert joined["Odd"]["seconds"] is None                 # no price, never a zero
    text = service.report(service.join(data, capture, implemented), 10)
    assert "(Device's, shared)" in text and "2 devices run Device::ProcessWithInterrupt: 0.020 s together" in text
    assert "(no ProcessWithInterrupt found for Odd)" in text


# ---- exit and memory censuses ----

def _exits(path, rows):
    path.write_bytes(b"".join(exits.EXIT.pack(pc, 0, *values) for pc, values in sorted(rows.items())))


def _memory(path, scale):
    path.write_text(json.dumps({"schema": "op1.memory.v1", "accesses": 30 * scale, "regions": [
        {"name": "SDRAM", "base": 0, "end": 0x7ffffff, "reads": [0, 0, 20 * scale], "writes": [0, 0, 8 * scale]},
        {"name": "NFC", "base": 0xffc03700, "end": 0xffc0374f, "reads": [0, 2 * scale, 0], "writes": [0, 0, 0]},
        {"name": "unmapped", "base": 0, "end": 0, "reads": [0, 0, 0], "writes": [0, 0, 0]}]}))


def _exit_run(tmp_path):
    prefix = tmp_path / "census"
    _census(Path(f"{prefix}.pc.a"), [(0x100, 10, 40), (0x200, 5, 5)])
    _census(Path(f"{prefix}.pc.b"), [(0x100, 110, 440), (0x200, 25, 25)])
    _exits(Path(f"{prefix}.exit.a"), {0x100: (0, 0, 0, 20), 0x200: (1, 0, 0, 5)})
    _exits(Path(f"{prefix}.exit.b"), {0x100: (0, 2, 0, 220), 0x200: (11, 2, 1, 25)})
    Path(f"{prefix}.edge.a").write_bytes(b"")
    Path(f"{prefix}.edge.b").write_bytes(census.EDGE.pack(0x100, 0x100, 98) + census.EDGE.pack(0x100, 0x200, 2))
    t = _trace([_mark("a", 0, 0, 0),
                {**_translate(10, 1, 1), "args": {"cpu_ns": 1, "pc": 0x100, "terminator": "jump"}},
                {**_translate(20, 1, 1), "args": {"cpu_ns": 1, "pc": 0x200, "terminator": "loop-setup"}},
                _mark("b", 1000, 0, 0)])
    return prefix, t


def test_exit_census_splits_runs_by_why_the_host_was_needed(tmp_path):
    prefix, t = _exit_run(tmp_path)
    data = exits.exits(prefix, t, "a", "b")
    rows = {b["pc"]: b for b in data["blocks"]}
    assert rows[0x100] == {"pc": 0x100, "runs": 100, "packets": 400, "mmio": 0, "host": 2, "both": 0, "free": 98,
                           "accesses": 200, "terminator": "jump"}
    assert rows[0x200]["free"] == 20 - 10 - 2 - 1 and rows[0x200]["terminator"] == "loop-setup"
    text = exits.exits_report(data, census.edge_phase(prefix, "a", "b"), 10, census.Symbols(None))
    assert "not needed (no MMR, nothing the guest saw):              105" in text
    assert "jump                     1           100            98  98.0%" in text
    assert "0x00000100          100           98  98.0% jump             0x00000100  98.0%" in text


def test_exit_census_cannot_need_the_host_more_often_than_a_block_ran(tmp_path):
    prefix, t = _exit_run(tmp_path)
    _exits(Path(f"{prefix}.exit.b"), {0x100: (0, 200, 0, 220), 0x200: (11, 2, 1, 25)})
    with pytest.raises(ValueError, match="needed the host, but it ran"):
        exits.exits(prefix, t, "a", "b")
    _exits(Path(f"{prefix}.exit.b"), {0x100: (0, 2, 0, 220), 0x300: (1, 0, 0, 0)})
    with pytest.raises(ValueError, match="never ran"):
        exits.exits(prefix, t, "a", "b")


def test_memory_census_by_device_and_block(tmp_path):
    prefix, t = _exit_run(tmp_path)
    _memory(Path(f"{prefix}.memory.a"), 1)
    _memory(Path(f"{prefix}.memory.b"), 4)
    memory = exits.memory_phase(prefix, "a", "b")
    assert memory["accesses"] == 90 and [r["name"] for r in memory["regions"]] == ["SDRAM", "NFC"]
    text = exits.memory_report(memory, exits.exits(prefix, t, "a", "b"), 5, census.Symbols(None))
    assert "SDRAM              0x00000000-0x07ffffff" in text and "unmapped" not in text
    assert "0x00000100          100           200      2.0" in text
    with pytest.raises(ValueError, match="went down"):
        exits.memory_phase(prefix, "b", "a")


def test_census_command_reports_exits_and_memory(tmp_path, capsys):
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    prefix, t = _exit_run(run_dir)
    (run_dir / "trace.json").write_text(json.dumps(t))
    _memory(Path(f"{prefix}.memory.a"), 1)
    _memory(Path(f"{prefix}.memory.b"), 2)
    assert cli.main(["census", str(run_dir), "--phase", "a:b", "--exits"]) == 0
    assert "block ends in" in capsys.readouterr().out
    assert cli.main(["census", str(run_dir), "--phase", "a:b", "--memory"]) == 0
    assert "guest loads and stores" in capsys.readouterr().out


def test_logging_is_its_own_bucket():
    categories = Categories.load()
    assert categories.bucket(["vsnprintf", "LogImpl(LogLevel, char const*, __va_list_tag*)",
                              "LogDebug(char const*, ...)", "get_device(unsigned int, ...)",
                              "Emulator::MemoryRead32(unsigned int)"]) == "logging"


# ---- diff and ab phases ----

def _census_dir(path, blocks, mmio, marks):
    path.mkdir()
    for mark, rows in blocks.items():
        _census(path / f"census.pc.{mark}", rows)
        _census(path / f"census.mmio.{mark}", mmio[mark])
    (path / "log.txt").write_text("\n".join(map(_mark_line, marks)) + "\n")


def test_diff_census_names_the_blocks_and_phase_that_changed(tmp_path, capsys):
    marks = [_mark_entry("a", 10), _mark_entry("b", 20)]
    _census_dir(tmp_path / "A", {"a": [(0x100, 1, 4)], "b": [(0x100, 5, 20), (0x200, 3, 3)]},
                {"a": [], "b": [(0xffc00300, 2, 0)]}, marks)
    _census_dir(tmp_path / "B", {"a": [(0x100, 1, 4)], "b": [(0x100, 9, 24), (0x300, 2, 2)]},
                {"a": [], "b": [(0xffc00300, 3, 0)]}, [marks[0], _mark_entry("b", 21)])
    data = diff.census_diff(tmp_path / "A", tmp_path / "B", "a", "b")
    rows = {r["key"]: r for r in data["blocks"]}
    assert rows[0x100]["delta"] == [4, 4] and rows[0x200]["only"] == "a" and rows[0x300]["only"] == "b"
    assert data["first_difference"]["mark"] == "b" and data["first_difference"]["after"] == "a"
    text = diff.census_report(data, 10, census.Symbols(None))
    assert "guest work first differs in a -> b" in text
    assert "0x00000300            0            2             0             2           +2  (only in B)" in text
    assert cli.main(["diff", "census", str(tmp_path / "A"), str(tmp_path / "A"), "--phase", "a:b",
                     "--json", str(tmp_path / "same.json")]) == 0
    out = capsys.readouterr().out
    assert "guest work identical at every mark" in out and "every block ran the same number of times" in out
    assert json.loads((tmp_path / "same.json").read_text())["blocks"] == []
    assert cli.main(["diff", "census", str(tmp_path / "A"), str(tmp_path / "B")]) == 2   # no phase


def _compile_run(path, blocks):
    """A run directory whose trace compiled `blocks`: (pc, hash, ir, bytes)."""
    path.mkdir()
    events = [_mark("a", 0, 0, 0)]
    for i, (pc, digest, ir, size) in enumerate(blocks):
        events.append({**_translate(10 + i, 1, 1000), "args": {"pc": pc, "ok": True, "cpu_ns": 1000 * (i + 1),
                                                                "ir_insns": ir, "object_bytes": size,
                                                                "object_hash": digest}})
    events.append(_mark("b", 1000, 0, 0))
    (path / "trace.json").write_text(json.dumps(_trace(events)))


def test_diff_compile_aligns_blocks_by_code_and_weighs_them_by_work(tmp_path, capsys):
    _compile_run(tmp_path / "A", [(0x100, "1" * 16, 10, 40), (0x200, "2" * 16, 20, 80), (0x400, "4" * 16, 5, 9)])
    _compile_run(tmp_path / "B", [(0x100, "1" * 16, 10, 40), (0x200, "9" * 16, 12, 50), (0x300, "3" * 16, 7, 9)])
    data = diff.compile_diff(tmp_path / "A", tmp_path / "B", ("a", "b"))
    assert data["counts"] == {"same": 1, "changed": 1, "mixed": 0, "only a": 1, "only b": 1}
    weights = tmp_path / "W"
    weights.mkdir()
    _census(weights / "census.pc.a", [])
    _census(weights / "census.pc.b", [(0x100, 10, 30), (0x200, 50, 70)])
    weighed = diff.compile_diff(tmp_path / "A", tmp_path / "B", ("a", "b"), weights)
    assert weighed["changed_packets"] == 70 and weighed["unchanged_packets"] == 30
    text = diff.compile_report(weighed, 10, census.Symbols(None))
    assert "blocks whose code changed executed 70.0% of the phase's packets" in text
    assert "0x00000200      20      12       80       50" in text
    assert "compiled only in A: 0x00000400" in text and "compiled only in B: 0x00000300" in text
    with pytest.raises(ValueError, match="give --phase"):
        diff.compile_diff(tmp_path / "A", tmp_path / "B", None, weights)
    assert cli.main(["diff", "compile", str(tmp_path / "A"), str(tmp_path / "B"), "--phase", "a:b",
                     "--census", str(weights)]) == 0


def test_ab_reports_each_phase_and_the_shape(files, tmp_path, capsys):
    out = tmp_path / "ab"
    out.mkdir()
    slow = tmp_path / "slow.sh"
    slow.write_text(f"#!/bin/sh\nFAKE_SCALE=1.2 exec {FAKE} \"$@\"\n")
    slow.chmod(0o755)
    report = ab.compare(_side("base", files, out), ab.Side("new", snapshot_binary(slow, out), [], sha256_file(slow)),
                        spec(files), 4, out, ("start", "main-frame"), 2.0, False)
    assert report["schema"] == ab.SCHEMA
    phases = {(p["begin"], p["end"]): p for p in report["phases"]}
    assert set(phases) == {("start", "bootrom"), ("bootrom", "main-display"), ("main-display", "main-frame")}
    assert phases[("bootrom", "main-display")]["noncompile_cpu_ms"]["mean_pct"] == pytest.approx(20.0)
    assert phases[("bootrom", "main-display")]["compile_cpu_ms"]["median_delta_ms"] == 0
    assert report["shape"]["base"]["runs"] == report["shape"]["new"]["runs"]
    assert report["shape"]["base"]["packets_per_run"] == pytest.approx(3.0, rel=0.01)
    cli._print_ab(report, out)
    printed = capsys.readouterr().out
    assert "shape at main-frame (base -> new): runs 1,000 -> 1,000" in printed
    assert "bootrom -> main-display" in printed and "+20.0%" in printed


def _versioned_run(path, versions):
    """A run whose block 0x100 is compiled once per (ts_us, hash) in
    `versions`; marks a at 50 us and b at 1000 us."""
    path.mkdir()
    events = [_mark("a", 50, 0, 0), _mark("b", 1000, 0, 0)]
    for ts, digest in versions:
        events.append({**_translate(ts, 1, 1000), "args": {"pc": 0x100, "ok": True, "cpu_ns": 1000,
                                                            "ir_insns": 5, "object_bytes": 9, "object_hash": digest}})
    (path / "trace.json").write_text(json.dumps(_trace(sorted(events, key=lambda e: e["ts"]))))


def test_diff_compile_does_not_call_recompiled_code_unchanged(tmp_path):
    # Different code for most of the phase, then the same after a recompile:
    # which version ran how much is unknown, so it is neither same nor changed.
    _versioned_run(tmp_path / "A", [(10, "1" * 16), (500, "3" * 16)])
    _versioned_run(tmp_path / "B", [(10, "2" * 16), (500, "3" * 16)])
    weights = tmp_path / "W"
    weights.mkdir()
    _census(weights / "census.pc.a", [])
    _census(weights / "census.pc.b", [(0x100, 10, 1000)])
    data = diff.compile_diff(tmp_path / "A", tmp_path / "B", ("a", "b"), weights)
    assert data["blocks"][0]["state"] == "mixed" and data["blocks"][0]["a"]["versions"] == 2
    assert data["mixed_packets"] == 1000 and data["unchanged_packets"] == 0
    assert "unknown (several versions that differ" in diff.compile_report(data, 5, census.Symbols(None))
    # The same versions in the same order are the same code; a version
    # replaced before the phase began never ran in it.
    _versioned_run(tmp_path / "C", [(10, "9" * 16), (20, "1" * 16), (500, "3" * 16)])
    assert diff.compile_diff(tmp_path / "A", tmp_path / "C", ("a", "b"))["blocks"][0]["state"] == "same"


@pytest.mark.parametrize("phase,match", [(("nowhere", "b"), "not in the trace"), (("b", "a"), "empty"),
                                         (("b", "b"), "empty")])
def test_diff_compile_checks_its_phase(tmp_path, phase, match):
    _versioned_run(tmp_path / "A", [(10, "1" * 16)])
    with pytest.raises(ValueError, match=match):
        diff.compile_diff(tmp_path / "A", tmp_path / "A", phase)


def test_first_difference_follows_both_firing_orders():
    def marks(*names):
        return [parse_mark(f"[mark] {n} pc=0x0 wall_ms=0 cpu_ms=0 runs=1 packets=1 cycles=1 translated=0 "
                           "mmio_r=0 mmio_w=0 events=0") for n in names]
    found = diff.first_difference(marks("a", "b"), marks("a", "middle", "b"))
    assert found["after"] == "a" and found["mark"] == "middle"
    assert "A fired b and B fired middle" in found["lines"][0]
    assert diff.first_difference(marks("a", "b"), marks("a"))["lines"] == ["after a, A fired b and B fired nothing"]
    assert diff.first_difference(marks("a", "b"), marks("a", "b")) is None


def test_ab_phase_delta_is_the_median_of_an_even_number_of_pairs():
    pairs = [{"base": {"phases": [{"begin": "a", "end": "b", "noncompile_cpu_ms": 100.0, "compile_cpu_ms": 10.0}]},
              "new": {"phases": [{"begin": "a", "end": "b", "noncompile_cpu_ms": 100.0 + d, "compile_cpu_ms": 10.0}]}}
             for d in (-3000.0, -1000.0, 1000.0, 3000.0)]
    assert ab._phase_deltas(pairs, 1)[0]["noncompile_cpu_ms"]["median_delta_ms"] == 0


def test_mmio_first_json_has_the_records(tmp_path):
    run_dir = tmp_path / "run"
    run_dir.mkdir()
    prefix = run_dir / "census"
    _census(Path(f"{prefix}.mmio.a"), [])
    _census(Path(f"{prefix}.mmio.b"), [(0xffc00c40, 0, 2)])
    Path(f"{prefix}.mmiofirst.b").write_bytes(census.FIRST.pack(0xffc00c40, 0x200, 4_000_000, 4_000_000))
    (run_dir / "log.txt").write_text("\n".join(map(_mark_line, [{**_mark_entry("a", 1), "cycles": 1000},
                                                                {**_mark_entry("b", 2), "cycles": 5_000_000}])))
    out = tmp_path / "first.json"
    assert cli.main(["census", str(run_dir), "--phase", "a:b", "--mmio-first", "--json", str(out)]) == 0
    assert json.loads(out.read_text())["mmio_first"] == [
        {"addr": 0xffc00c40, "first_cycles": 4_000_000, "first_block": 0x200, "reads": 0, "writes": 2}]


@pytest.mark.parametrize("a,b,state", [
    ([(10, "1" * 16), (500, "1" * 16)], [(10, "1" * 16)], "same"),                      # the same code compiled again
    ([(10, "1" * 16), (500, "1" * 16)], [(10, "2" * 16), (600, "2" * 16)], "changed"),  # one version a side
    ([(10, "1" * 16), (500, "3" * 16)], [(10, "1" * 16), (500, "1" * 16), (600, "3" * 16)], "same"),
])
def test_diff_compile_counts_versions_of_code_not_compilations(tmp_path, a, b, state):
    _versioned_run(tmp_path / "A", a)
    _versioned_run(tmp_path / "B", b)
    weights = tmp_path / "W"
    weights.mkdir()
    _census(weights / "census.pc.a", [])
    _census(weights / "census.pc.b", [(0x100, 10, 1000)])
    data = diff.compile_diff(tmp_path / "A", tmp_path / "B", ("a", "b"), weights)
    assert data["blocks"][0]["state"] == state and data["mixed_packets"] == 0


def test_first_difference_judges_a_missing_counter_over_the_whole_runs():
    def mark(name, irq):
        return parse_mark(f"[mark] {name} pc=0x0 wall_ms=0 cpu_ms=0 runs=1 packets=1 cycles=1 translated=0 "
                          f"mmio_r=0 mmio_w=0 events=0{irq}")
    a = [mark("a", " irq=0"), mark("b", " irq=1")]
    b = [mark("a", " irq=0"), mark("b", "")]
    found = diff.first_difference(a, b)
    assert found["mark"] == "b" and any("irq: missing" in line for line in found["lines"])
    # A binary without the counter at all is still compared on the rest.
    older = [mark("a", ""), mark("b", "")]
    assert diff.first_difference(older, [mark("a", ""), mark("b", "")]) is None
    assert diff.first_difference([mark("a", " irq=0"), mark("b", " irq=1")], older) is None


# ---- capture sets, perf diff, lost samples ----

def _report(seconds, rtc, fir_self, window=("a", "b"), lost=None):
    """A perf report as report.analyze() returns it, with the parts capture_set reads."""
    return {"window": list(window), "event": "cycles:u", "rate": ["-c", "4000000"], "call_graph": "lbr",
            "execution_cpu_s": seconds, "inclusive_valid": True, "record": lost,
            "samples": {"execution": {"count": 100, "weight": 100}},
            "buckets": [{"bucket": "rtc", "seconds": rtc}, {"bucket": "dma", "seconds": 1.0}],
            "blocks": [{"block": "bb_0x01a5e27e", "self_s": fir_self, "incl_s": fir_self * 4}],
            "host_self": [{"symbol": "RTC::Tick()", "seconds": rtc / 2}],
            "host_inclusive": [{"symbol": "RTC::ProcessWithInterrupt(int)", "seconds": rtc}]}


def test_a_capture_set_reports_means_and_intervals_from_three_captures():
    reports = [_report(10.0, 2.0, 1.0), _report(10.2, 2.2, 1.1), _report(9.8, 1.8, 0.9)]
    data = capture_set.aggregate([Path("c0"), Path("c1"), Path("c2")], reports)
    rtc = {r["bucket"]: r for r in data["buckets"]}["rtc"]
    assert rtc["mean"] == pytest.approx(2.0) and rtc["values"] == [2.0, 2.2, 1.8]
    half = stats.t975(2) * 0.2 / 3 ** 0.5
    assert rtc["ci95"] == pytest.approx([2.0 - half, 2.0 + half])
    incl = {r["block"]: r for r in data["blocks_incl"]}["bb_0x01a5e27e"]
    assert incl["mean"] == pytest.approx(4.0)
    assert "95% t interval" in capture_set.format_set(data, 5)
    two = capture_set.aggregate([Path("c0"), Path("c1")], reports[:2])
    assert all(r["ci95"] is None for r in two["buckets"])
    assert "an interval needs 3 or more" in capture_set.format_set(two, 5)


def test_a_perf_diff_gives_welch_intervals_only_with_enough_captures():
    a = [_report(10.0, 2.0, 1.0), _report(10.0, 2.1, 1.0), _report(10.0, 1.9, 1.0)]
    b = [_report(9.0, 1.0, 1.0), _report(9.0, 1.1, 1.0), _report(9.0, 0.9, 1.0)]
    caps = [Path(f"a{i}") for i in range(3)]
    other = [Path(f"b{i}") for i in range(3)]
    data = _diff_sets(caps, a, other, b)
    rtc = {r["bucket"]: r for r in data["buckets"]}["rtc"]
    assert rtc["delta"] == pytest.approx(-1.0) and rtc["ci95"][1] < 0
    dma = {r["bucket"]: r for r in data["buckets"]}["dma"]
    assert dma["ci95"] == [0.0, 0.0]
    assert "95% Welch interval" in capture_set.format_diff(data, 5)
    single = _diff_sets(caps[:1], a[:1], other[:1], b[:1])
    assert all(r["ci95"] is None for r in single["buckets"])
    assert "a single capture's difference is not evidence" in capture_set.format_diff(single, 5)
    with pytest.raises(ValueError, match="window"):
        _diff_sets(caps, a, other, [_report(9.0, 1.0, 1.0, window=("a", "c"))] * 3)
    with pytest.raises(ValueError, match="both sides"):
        _diff_sets(caps, a, caps, b)


def _diff_sets(ca, a, cb, b, monkeypatch=None):
    # diff_sets reads each set's run marks for the workload check: give it equal ones.
    from unittest import mock
    with mock.patch.object(perf_report, "_run_entry", lambda path: {"marks": [_mark_entry("b", 10)]}):
        return capture_set.diff_sets(ca, a, cb, b)


def test_captures_under_a_set_directory_and_the_same_run_check(tmp_path, monkeypatch):
    for i in range(2):
        (tmp_path / "set" / f"capture{i:02d}").mkdir(parents=True)
        (tmp_path / "set" / f"capture{i:02d}" / "capture.json").write_text("{}")
    assert [p.name for p in capture_set.captures_under(tmp_path / "set")] == ["capture00", "capture01"]
    assert capture_set.captures_under(tmp_path / "set" / "capture01") == [tmp_path / "set" / "capture01"]
    with pytest.raises(ValueError, match="no capture"):
        capture_set.captures_under(tmp_path)
    reports = iter([_report(10.0, 2.0, 1.0), _report(10.0, 2.0, 1.0, window=("a", "c"))])
    monkeypatch.setattr(perf_report, "analyze", lambda *a, **k: next(reports))
    with pytest.raises(ValueError, match="window"):
        capture_set.load(capture_set.captures_under(tmp_path / "set"), Categories.load(), perf_report.Labels())
    checked = []
    monkeypatch.setattr(perf_report, "analyze", lambda *a, **k: _report(10.0, 2.0, 1.0))
    monkeypatch.setattr(perf_report, "check_same_run", lambda a, b: checked.append((a.name, b.parent.name)))
    capture_set.load(capture_set.captures_under(tmp_path / "set"), Categories.load(), perf_report.Labels())
    assert checked == [("capture00", "capture01")]


def test_lost_samples_are_counted_from_the_records_not_the_record_count():
    stats_out = ["           TOTAL events:      34521", "          SAMPLE events:      28982  (84.0%)",
                 "            LOST events:          3  ( 0.0%)", "    LOST_SAMPLES events:          2  ( 0.0%)"]
    # As perf report -D prints them: three overflows of 2, 16 and 128 samples,
    # then per-event summaries of the same 146.
    dump = ["2514044432043386 0x5e080 [0x30]: PERF_RECORD_LOST: id:4026: lost:2",
            "2514045191034226 0xd35e0 [0x30]: PERF_RECORD_LOST: id:4032: lost:16",
            "2514045341026017 0xe7f58 [0x30]: PERF_RECORD_LOST: id:4032: lost:128",
            "0 0x15ef08 [0x28]: PERF_RECORD_LOST_SAMPLES: id:4026: lost samples :2",
            "0 0x15ef30 [0x28]: PERF_RECORD_LOST_SAMPLES: id:4032: lost samples :144",
            "2514044432043999 0x5e0b0 [0x18]: PERF_RECORD_THROTTLE: unhandled!"]
    found = perf.parse_record_stats(stats_out, dump)
    assert found["samples"] == 28982 and found["lost"] == 146 and found["throttle"] == 1
    assert "throttled the sampling 1 times" in found["warning"]
    # Without summaries the overflows are the count: the review's case, one
    # record of 261 lost against 519 samples.
    one = perf.parse_record_stats(["SAMPLE events: 519", "LOST events: 1"], ["x: PERF_RECORD_LOST: id:1: lost:261"])
    assert one["lost"] == 261 and "261 of 780 samples lost" in one["warning"]
    clean = perf.parse_record_stats(["SAMPLE events: 10"], [])
    assert clean == {"samples": 10, "lost": 0, "throttle": 0, "warning": None}


def test_host_symbols_keep_their_name_across_binaries_and_jit_dumps():
    assert perf_report.dso_label("op1emu-0123456789ab") == perf_report.dso_label("op1emu-ba9876543210") == "op1emu"
    assert perf_report.dso_label("jitted-4123-7.so") == perf_report.dso_label("jitted-99-12.so") == "jitted"
    assert perf_report.dso_label("libc.so.6") == "libc.so.6"
    # Two binaries' captures: the same function is one row, not a loss and a gain.
    def report(dso):
        r = _report(10.0, 2.0, 1.0)
        r["host_self"] = [{"symbol": f"RTC::Tick() ({perf_report.dso_label(dso)})", "seconds": 10.0}]
        return r
    data = _diff_sets([Path(f"a{i}") for i in range(3)], [report("op1emu-0123456789ab")] * 3,
                      [Path(f"b{i}") for i in range(3)], [report("op1emu-ba9876543210")] * 3)
    assert [(r["symbol"], r["delta"]) for r in data["host_self"]] == [("RTC::Tick() (op1emu)", 0.0)]


def test_a_capture_set_refuses_empty_and_repeated_captures(tmp_path, monkeypatch):
    for i in range(2):
        (tmp_path / f"c{i}").mkdir()
        (tmp_path / f"c{i}" / "capture.json").write_text("{}")
    monkeypatch.setattr(perf_report, "check_same_run", lambda a, b: None)
    empty = {**_report(10.0, 2.0, 1.0), "samples": {"execution": {"count": 0, "weight": 0}}}
    reports = iter([_report(10.0, 2.0, 1.0), empty])
    monkeypatch.setattr(perf_report, "analyze", lambda *a, **k: next(reports))
    with pytest.raises(ValueError, match="no execution samples"):
        capture_set.load([tmp_path / "c0", tmp_path / "c1"], Categories.load(), perf_report.Labels())
    monkeypatch.setattr(perf_report, "analyze", lambda *a, **k: _report(10.0, 2.0, 1.0))
    with pytest.raises(ValueError, match="given twice"):
        capture_set.load([tmp_path / "c0", tmp_path / "c0", tmp_path / "c0"], Categories.load(), perf_report.Labels())
    with pytest.raises(ValueError, match="given twice"):     # a capture and its parent directory
        capture_set.load(capture_set.captures_under(tmp_path) + [tmp_path / "c1"], Categories.load(),
                         perf_report.Labels())


def test_ns_per_run_order_leaves_out_rarely_run_blocks():
    joined = {"runs": 1100, "outside_blocks_s": 0.0, "outside_blocks_ns_per_run": 0.0, "sampled_not_in_census_s": 0.0,
              "blocks": [{"block": "rare", "runs": 3, "packets": 3, "seconds": 0.001, "ns_per_run": 333333.0,
                          "ns_per_packet": 333333.0},
                         {"block": "hot", "runs": 1097, "packets": 5000, "seconds": 0.002, "ns_per_run": 1823.0,
                          "ns_per_packet": 400.0}]}
    lines = "\n".join(perf_report._census_lines(joined, 5, "ns-per-run"))
    assert "1 blocks that ran fewer than 1000 times are left out" in lines and "rare" not in lines
    assert "rare" in "\n".join(perf_report._census_lines(joined, 5, "seconds"))
    assert "rare" in "\n".join(perf_report._census_lines(joined, 5, "ns-per-run", min_runs=1))


def test_ab_perf_captures_both_sides_alternately_after_the_pairs(files, tmp_path, monkeypatch, capsys):
    recorded = []
    def fake_record(spec, directory, window, event, period, freq, call_graph):
        recorded.append((directory.parent.name, directory.name, event, call_graph))
        result = run(spec, directory / "run")
        (directory / "capture.json").write_text("{}")
        return result
    monkeypatch.setattr(perf, "record", fake_record)
    monkeypatch.setattr(perf, "preflight", lambda *a: None)
    monkeypatch.setattr(perf_report, "analyze", lambda *a, **k: _report(10.0, 2.0, 1.0, window=("start", "main-frame")))
    monkeypatch.setattr(perf_report, "check_same_run", lambda a, b: None)
    assert cli.main(["ab", "--base-bin", str(FAKE), "--nand", str(files / "nand.bin"), "--otp", str(files / "otp.bin"),
                     "--marks", str(files / "marks.json"), "--force", "--out", str(tmp_path / "ab"), "--aa",
                     "--timeout", "30", "--progress-timeout", "5", "--perf", "2"]) in (0, 1)
    assert [(side, name) for side, name, _, _ in recorded] == [("base", "capture00"), ("new", "capture00"),
                                                                ("new", "capture01"), ("base", "capture01")]
    assert recorded[0][2:] == ("cycles:u", "lbr")
    assert "where the time went, 2 perf captures a side" in capsys.readouterr().out
    assert json.loads((tmp_path / "ab" / "perf" / "diff.json").read_text())["schema"] == capture_set.DIFF_SCHEMA


def test_ab_perf_checks_perf_before_the_timing_runs(files, tmp_path, monkeypatch):
    monkeypatch.setattr(perf, "preflight", lambda *a: "perf may not record: set kernel.perf_event_paranoid=2")
    with pytest.raises(SystemExit, match="--perf: perf may not record"):
        cli.main(["ab", "--base-bin", str(FAKE), "--nand", str(files / "nand.bin"), "--otp", str(files / "otp.bin"),
                  "--marks", str(files / "marks.json"), "--force", "--out", str(tmp_path / "ab"), "--aa", "--perf", "3"])
    assert not (tmp_path / "ab").exists()


# ---- perf's build-id cache ----

def test_a_capture_keeps_perfs_build_id_cache_inside_itself(files, tmp_path, monkeypatch):
    # perf copies every DSO it samples into ~/.debug, which outlives the
    # capture: a capture gets its own cache, used by every perf command on it.
    seen = {}
    def fake_run(spec, directory):
        seen["prefix"] = spec.prefix
        return run(spec_without_prefix(spec), directory)
    def spec_without_prefix(spec):
        from dataclasses import replace
        return replace(spec, prefix=[])
    monkeypatch.setattr(perf, "run", fake_run)
    # perf inject runs after a reached run: recorded here, so the test needs no perf.
    class Done:
        returncode, stderr = 0, ""
    commands = []
    monkeypatch.setattr(perf.subprocess, "run", lambda command, **kw: commands.append(command) or Done())
    capture = tmp_path / "capture"
    perf.record(spec(files), capture, ("start", "main-frame"), "cycles:u", 4000000, None)
    cache = (capture / perf.BUILDID_DIR).resolve()
    assert cache.is_dir() and seen["prefix"][:4] == ["perf", "--buildid-dir", str(cache), "record"]
    assert [c[:4] for c in commands] == [["perf", "--buildid-dir", str(cache), "inject"]]
    assert perf.perf_command(capture) == ["perf", "--buildid-dir", str(cache)]
    old = tmp_path / "old-capture"
    old.mkdir()
    assert perf.perf_command(old) == ["perf"]          # a capture from before keeps using ~/.debug


def test_the_permission_probe_caches_nothing(monkeypatch):
    calls = []
    class Done:
        returncode, stderr = 0, ""
    monkeypatch.setattr(perf.subprocess, "run", lambda command, **kw: calls.append(command) or Done())
    assert perf.preflight("cycles:u", ["-c", "4000000"]) is None
    assert "--no-buildid-cache" in calls[0]
