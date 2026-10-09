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

from op1prof import ab, stats, trace  # noqa: E402
from op1prof.marks import parse_mark, workload_diff, parse_marks  # noqa: E402
from op1prof.runner import RunSpec, run, snapshot_binary, sha256_file  # noqa: E402

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


def test_snapshot_binary_is_content_named(files, tmp_path):
    copy = snapshot_binary(FAKE, tmp_path / "out")
    assert copy.name == f"op1emu-{sha256_file(FAKE)[:12]}" and os.access(copy, os.X_OK)


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

