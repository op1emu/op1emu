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

from op1prof import ab, cli, machine, stats, trace, verify  # noqa: E402
from op1prof.marks import parse_mark, workload_diff, parse_marks  # noqa: E402
from op1prof.runner import RESERVED_ARGS, RunSpec, run, snapshot_binary, sha256_file  # noqa: E402

FAKE = Path(__file__).with_name("fake_op1emu.py")


@pytest.fixture
def files(tmp_path):
    os.chmod(FAKE, os.stat(FAKE).st_mode | stat.S_IXUSR)
    for name in ("nand.bin", "otp.bin", "marks.json"):
        (tmp_path / name).write_text("x")
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
    assert m.workload() == {"packets": 10, "cycles": 10, "mmio_r": 1, "mmio_w": 0, "events": 4}
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
    # The fake never fires lap-end: the run reaches --until, but the window
    # it was meant to sample did not close, so there is nothing to report.
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


def _capture(tmp_path, trace_data, marks=()):
    capture = tmp_path / "capture"
    (capture / "run").mkdir(parents=True)
    (capture / "run" / "trace.json").write_text(json.dumps(trace_data))
    (capture / "capture.json").write_text(json.dumps({
        "event": "cpu-clock:u", "precise": False, "rate": ["-F", "999"], "window": ["a", "b"],
        "run": {"marks": list(marks)}}))
    return capture


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
    (census_run / "log.txt").write_text("\n".join(map(_mark_line, marks)) + "\n")
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
    (census_run / "log.txt").write_text("\n".join(map(_mark_line, [marks[0], _mark_entry("b", 21)])) + "\n")
    with pytest.raises(ValueError, match="different guest work"):
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
    assert phase.compile_insns == 15_000_000 and phase.noncompile_insns == 90_000_000
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
    return sys_root


def test_hybrid_cpus_need_a_pinned_performance_core(hybrid):
    assert machine.core_types() == {"performance": {0, 1, 2, 3}, "efficiency": {4, 5}}
    assert machine.siblings(2) == [3] and machine.siblings(4) == []
    assert any("pass --cpu with a performance core (0-3)" in p for p in machine.preflight(None, 0, require_pin=True))
    assert not any("--cpu" in p for p in machine.preflight(None, 0))     # a single run may float
    assert any("efficiency core" in p for p in machine.preflight(4, 0))
    assert not any("core" in p for p in machine.preflight(2, 0, require_pin=True))


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
