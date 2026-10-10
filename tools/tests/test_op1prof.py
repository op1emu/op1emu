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

from op1prof import ab, cli, stats, trace, verify  # noqa: E402
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


def test_perf_fails_when_the_window_never_closed(files, monkeypatch, capsys):
    # The fake never fires lap-end: the run reaches --until, but the window
    # it was meant to sample did not close, so there is nothing to report.
    marks = files / "window.json"
    marks.write_text(json.dumps({"marks": [{"name": "bootrom"}, {"name": "lap-end"}, {"name": "main-display"}],
                                 "frame": {"name": "main-frame"}}))
    monkeypatch.setattr(perf, "preflight", lambda *a: None)
    monkeypatch.setattr(perf, "record", lambda spec, directory, *a: run(spec, directory))
    monkeypatch.setattr(perf, "report", lambda *a: pytest.fail("reported an unclosed window"))
    assert cli.main(["perf", "--binary", str(FAKE), "--nand", str(files / "nand.bin"), "--otp", str(files / "otp.bin"),
                     "--marks", str(marks), "--window", "bootrom:lap-end", "--force",
                     "--out", str(files / "p"), "--timeout", "30", "--progress-timeout", "5"]) == 1
    assert "did not both fire" in capsys.readouterr().err


def test_samples_are_attributed_outside_compilation():
    lines = [
        " 42 10.000000100:  7f00 bb_0x01a5e27e (/tmp/jitted-42-1.so)",
        " 42 10.000000200:  7f01 bb_0x01a5e27e (/tmp/jitted-42-1.so)",
        " 42 10.000000300:  4000 EmulatorMemory::read32 (/x/op1emu)",
        " 42 10.000000550:  7f02 bb_0x00000000 (/tmp/jitted-42-2.so)",   # inside a translate span
        " 43 10.000000300:  4000 glfwPollEvents (/x/libglfw.so)",          # another thread
        " 42 11.000000000:  7f00 bb_0x01a5e27e (/tmp/jitted-42-1.so)",    # after the window
        "unrelated line",
    ]
    samples = list(perf.parse_samples(lines))
    assert samples[0] == (42, 10_000_000_100, "bb_0x01a5e27e", "jitted-42-1.so")
    trace_data = {"cpu_thread_tid": 42, "traceEvents": [
        {"name": "a", "cat": "mark", "ts": 10_000_000.0},
        {"name": "translate", "ph": "X", "ts": 10_000_000.5, "dur": 0.1},
        {"name": "b", "cat": "mark", "ts": 10_000_001.0},
    ]}
    result = perf.attribute(samples, trace_data, ("a", "b"))
    assert result["counts"] == {"execution": 3, "compile": 1, "other_threads": 1, "outside_window": 1}
    assert result["blocks"]["bb_0x01a5e27e"] == 2
    assert result["symbols"]["EmulatorMemory::read32 (op1emu)"] == 1


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
    assert "0x00000300" in text.splitlines()[5] and "main" in text  # most packets first
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
