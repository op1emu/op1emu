"""One emulator run: isolated directory, fixed inputs, watched to completion."""
from __future__ import annotations

import hashlib
import os
import selectors
import shutil
import signal
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

from . import machine
from .marks import Mark, parse_mark

REPO = Path(__file__).resolve().parents[2]
# Fixed sensors, applied before the CPU starts (--deterministic). The long wait
# keeps the frontend idle; --profile-until ends the run.
DEFAULT_SCRIPT = "accel 100 -100 500\nvolume 128\nwait 86400000\n"
# What run() sets on every run. The emulator keeps the last value of a
# repeated option, so an extra argument naming one would override it (say
# --nand-rw, or a second --profile-until); "--" would turn the rest into
# positional paths. Extra arguments may not use these.
RESERVED_ARGS = ("--", "--nand-rw", "--nand-snapshot", "--headless", "--deterministic", "--input-script",
                 "--profile-marks", "--profile-until", "--profile-trace")
# How long an emulator that closed its output may take to exit.
EXIT_GRACE_S = 60.0


def reserved_args(args: List[str]) -> List[str]:
    return [arg for arg in args if arg in RESERVED_ARGS]


def sha256_file(path: str | Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for chunk in iter(lambda: file.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def snapshot_binary(binary: str | Path, out_dir: Path) -> Path:
    """Copy the binary aside so a rebuild during a batch cannot change what
    is measured. Named by content, so identical binaries share one copy.
    Absolute, because the run starts it from its own directory."""
    digest = sha256_file(binary)
    target = out_dir / "bin" / f"op1emu-{digest[:12]}"
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(binary, target)
    return target.resolve()


@dataclass
class RunSpec:
    binary: Path
    nand: Path
    otp: Path
    marks: Path
    until: str
    args: List[str] = field(default_factory=list)
    script: str = DEFAULT_SCRIPT
    timeout_s: float = 3600.0
    progress_timeout_s: float = 900.0
    cpu: Optional[int] = None
    trace: bool = True
    census: bool = False
    prefix: List[str] = field(default_factory=list)   # e.g. perf record ... --
    env: dict = field(default_factory=dict)           # added after cleaning

    def __post_init__(self) -> None:
        reserved = reserved_args(self.args)
        if reserved:
            raise ValueError(f"extra emulator arguments may not set what every run fixes: {reserved}")


@dataclass
class RunResult:
    directory: Path
    command: List[str]
    returncode: Optional[int]
    marks: List[Mark]
    error: Optional[str]
    elapsed_s: float
    before: dict
    after: dict

    @property
    def reached(self) -> bool:
        return self.error is None and any(m.name == self.until for m in self.marks)

    until: str = ""

    @property
    def trace_path(self) -> Path:
        return self.directory / "trace.json"

    def to_json(self) -> dict:
        return {
            "directory": str(self.directory), "command": self.command,
            "returncode": self.returncode, "error": self.error,
            "elapsed_s": self.elapsed_s, "reached": self.reached,
            "marks": [{"name": m.name, "pc": m.pc, "wall_ms": m.wall_ms, "cpu_ms": m.cpu_ms,
                       **m.counters} for m in self.marks],
            "machine_before": self.before, "machine_after": self.after,
        }


def clean_env() -> dict:
    """The parent's environment without anything the emulator or LLVM would
    read as configuration; the run is configured only by its command line."""
    return {key: value for key, value in os.environ.items()
            if not key.startswith("OP1_") and key not in ("JITDUMPDIR", "DISPLAY")}


def _kill_group(proc: subprocess.Popen, grace_s: float) -> None:
    for sig, wait in ((signal.SIGTERM, grace_s), (signal.SIGKILL, 5.0)):
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            return
        try:
            proc.wait(timeout=wait)
            break
        except subprocess.TimeoutExpired:
            continue
    # Children that inherited the group (none expected) must not outlive us.
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def run(spec: RunSpec, directory: Path) -> RunResult:
    directory.mkdir(parents=True, exist_ok=False)
    shutil.copy2(spec.otp, directory / "otp.bin")  # the guest may write OTP
    (directory / "gui").symlink_to(REPO / "gui")  # gui/ui.json is opened relative to cwd
    (directory / "script.txt").write_text(spec.script)
    command = [str(spec.binary), str(spec.nand), "--nand-snapshot", "--headless", "--deterministic",
               "--input-script", "script.txt", "--profile-marks", str(spec.marks),
               "--profile-until", spec.until]
    if spec.trace:
        command += ["--profile-trace", "trace.json"]
    if spec.census:
        command += ["--profile-census", "census"]
    command += spec.args
    command = spec.prefix + command

    before = machine.snapshot(spec.cpu).to_json()
    marks: List[Mark] = []
    error: Optional[str] = None
    start = time.monotonic()
    log = open(directory / "log.txt", "wb")
    preexec = (lambda: os.sched_setaffinity(0, {spec.cpu})) if spec.cpu is not None else None
    proc = subprocess.Popen(command, cwd=directory, env={**clean_env(), **spec.env}, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            start_new_session=True, preexec_fn=preexec)
    try:
        os.set_blocking(proc.stdout.fileno(), False)
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)
        pending = b""
        last_progress = start
        while True:
            now = time.monotonic()
            if now - start > spec.timeout_s:
                error = f"timed out after {spec.timeout_s:g} s"
                break
            if now - last_progress > spec.progress_timeout_s:
                last = marks[-1].name if marks else "start of run"
                error = f"no new mark for {spec.progress_timeout_s:g} s after {last}"
                break
            if not selector.select(timeout=1.0):
                if proc.poll() is not None:
                    break
                continue
            chunk = proc.stdout.read(65536)
            if not chunk:  # EOF: the process closed stdout
                # Normally it is exiting; one that keeps running must not
                # outlast the deadlines (cleanup below kills it).
                remaining = max(0.0, spec.timeout_s - (time.monotonic() - start))
                try:
                    proc.wait(timeout=min(EXIT_GRACE_S, remaining))
                except subprocess.TimeoutExpired:
                    error = "closed its output but did not exit"
                break
            log.write(chunk)
            log.flush()  # keep log.txt current for anyone watching a long run
            pending += chunk
            *lines, pending = pending.split(b"\n")
            for line in lines:
                mark = parse_mark(line.decode(errors="replace"))
                if mark:
                    marks.append(mark)
                    last_progress = time.monotonic()
    except BaseException:
        error = error or "interrupted"
        raise
    finally:
        if proc.poll() is None:
            _kill_group(proc, grace_s=15.0)
        else:
            _kill_group(proc, grace_s=0.0)
        log.close()
    returncode = proc.returncode
    if error is None and returncode != 0:
        error = f"exit status {returncode}"
    if error is None and not any(m.name == spec.until for m in marks):
        error = f"mark {spec.until!r} never fired"
    return RunResult(directory=directory, command=command, returncode=returncode, marks=marks,
                     error=error, elapsed_s=time.monotonic() - start, before=before,
                     after=machine.snapshot(spec.cpu).to_json(), until=spec.until)
