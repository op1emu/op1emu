"""Machine state around a measurement, and the checks made before one.

Every condition here moved a historical measurement: memory pressure
stretched LLVM compilation 4.4x and swap reclaim stalled the CPU thread for
3-15 s; a second emulator time-shared the pinned CPU; a benchmark process left
running made two "batches" overlap. On a hybrid CPU an unpinned thread moves
between performance and efficiency cores, and a busy SMT sibling shares the
pinned core's execution units.
"""
from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass, asdict
from typing import Dict, List, Optional, Set

# Root of sysfs, replaceable by tests.
SYS = "/sys"
# An SMT sibling busier than this during a run shared the core noticeably.
SIBLING_BUSY = 0.10


def _meminfo() -> Dict[str, int]:
    values = {}
    try:
        with open("/proc/meminfo") as file:
            for line in file:
                key, rest = line.split(":", 1)
                values[key] = int(rest.split()[0]) * 1024
    except OSError:
        pass
    return values


def _read(path: str) -> Optional[str]:
    try:
        with open(path) as file:
            return file.read().strip()
    except OSError:
        return None


def _cpulist(text: Optional[str]) -> Set[int]:
    """`0-15,18` as a set."""
    cpus: Set[int] = set()
    for part in (text or "").split(","):
        if not part.strip():
            continue
        lo, _, hi = part.partition("-")
        cpus.update(range(int(lo), int(hi or lo) + 1))
    return cpus


def core_types() -> Dict[str, Set[int]]:
    """CPUs per core type on a hybrid Intel CPU; empty on any other."""
    found = {}
    for kind, pmu in (("performance", "cpu_core"), ("efficiency", "cpu_atom")):
        cpus = _cpulist(_read(f"{SYS}/devices/{pmu}/cpus"))
        if cpus:
            found[kind] = cpus
    return found if len(found) == 2 else {}


def core_type(cpu: int) -> Optional[str]:
    return next((kind for kind, cpus in core_types().items() if cpu in cpus), None)


def siblings(cpu: int) -> List[int]:
    """The other hardware threads of `cpu`'s core."""
    return sorted(_cpulist(_read(f"{SYS}/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")) - {cpu})


def throttle_count(cpu: int) -> Optional[int]:
    """Thermal throttling events of `cpu`'s core since boot."""
    text = _read(f"{SYS}/devices/system/cpu/cpu{cpu}/thermal_throttle/core_throttle_count")
    return int(text) if text and text.isdigit() else None


# Root of procfs, replaceable by tests.
PROC = "/proc"


def _cpu_times() -> Dict[int, tuple]:
    """(busy, total) jiffies per CPU from /proc/stat."""
    out = {}
    for line in (_read(f"{PROC}/stat") or "").splitlines():
        if not line.split():
            continue
        name, *fields = line.split()
        if name.startswith("cpu") and name[3:].isdigit():
            values = [int(v) for v in fields[:8]]
            idle = values[3] + values[4]  # idle + iowait
            out[int(name[3:])] = (sum(values) - idle, sum(values))
    return out


def busy_between(before: Dict[str, list], after: Dict[str, list]) -> Dict[str, float]:
    """Busy fraction of each CPU between two snapshots' `cpu_times`: what
    happened during a run, unlike processes' lifetime averages."""
    out = {}
    for cpu, (busy0, total0) in before.items():
        if cpu in after:
            busy, total = after[cpu][0] - busy0, after[cpu][1] - total0
            out[cpu] = busy / total if total > 0 else 0.0
    return out


def sibling_busy(before: dict, after: dict, cpu: Optional[int]) -> Optional[float]:
    """The busiest SMT sibling of the pinned CPU during a run (snapshots).
    An average over the whole run: a sibling busy for a short burst barely
    moves it."""
    if cpu is None:
        return None
    fractions = busy_between(before.get("cpu_times") or {}, after.get("cpu_times") or {})
    values = [f for c, f in fractions.items() if int(c) != cpu]
    return max(values) if values else None


def _pressure() -> Optional[float]:
    """Memory PSI 'some' avg10, percent."""
    text = _read("/proc/pressure/memory")
    match = re.search(r"some avg10=([0-9.]+)", text or "")
    return float(match.group(1)) if match else None


def busy_processes_on(cpu: int, own: List[int], threshold: float = 5.0) -> List[str]:
    """Other processes last scheduled on `cpu` and using noticeable CPU."""
    try:
        out = subprocess.run(["ps", "-eo", "pid,psr,pcpu,comm", "--no-headers"],
                             capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    busy = []
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and int(parts[1]) == cpu and float(parts[2]) >= threshold \
                and int(parts[0]) not in own:
            busy.append(f"{parts[0]} {parts[3]} {parts[2]}%")
    return busy


def running_emulators(exclude: List[int]) -> List[str]:
    """Other op1emu processes: they share usbipd's port (a second instance
    busy-spins its event loop) and compete for CPU and memory."""
    found = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit() or int(entry) in exclude:
            continue
        try:
            exe = os.path.basename(os.readlink(f"/proc/{entry}/exe"))
        except OSError:
            continue
        if exe.startswith("op1emu"):
            found.append(f"{entry} {exe}")
    return found


@dataclass
class Snapshot:
    mem_available: int
    swap_used: int
    memory_pressure_avg10: Optional[float]
    loadavg: Optional[str]
    governor: Optional[str]
    cpu_mhz: Optional[float]
    busy_on_cpu: List[str]
    core_type: Optional[str] = None           # performance/efficiency on a hybrid CPU
    energy_performance_preference: Optional[str] = None
    no_turbo: Optional[str] = None
    throttle_count: Optional[int] = None      # the core's thermal throttling events since boot
    busy_on_siblings: List[str] = None
    cpu_times: Dict[str, list] = None         # (busy, total) jiffies of the pinned CPU and its siblings

    def to_json(self) -> dict:
        return asdict(self)


def measured_cpu(cpu: Optional[int]) -> Optional[int]:
    """The one CPU a run can execute on: --cpu, or the only CPU this process
    may use. None when the run may move between CPUs; per-CPU readings
    (frequency, throttling, the sibling's load) then mean nothing."""
    if cpu is not None:
        return cpu
    allowed = os.sched_getaffinity(0)
    return next(iter(allowed)) if len(allowed) == 1 else None


def snapshot(cpu: Optional[int]) -> Snapshot:
    info = _meminfo()
    cpu = measured_cpu(cpu)
    probe = cpu
    freq = _read(f"{SYS}/devices/system/cpu/cpu{probe}/cpufreq/scaling_cur_freq") if probe is not None else None
    return Snapshot(
        mem_available=info.get("MemAvailable", 0),
        swap_used=info.get("SwapTotal", 0) - info.get("SwapFree", 0),
        memory_pressure_avg10=_pressure(),
        loadavg=_read("/proc/loadavg"),
        governor=_read(f"{SYS}/devices/system/cpu/cpu{probe}/cpufreq/scaling_governor") if probe is not None else None,
        cpu_mhz=int(freq) / 1000 if freq and freq.isdigit() else None,
        busy_on_cpu=busy_processes_on(cpu, [os.getpid()]) if cpu is not None else [],
        core_type=core_type(probe) if probe is not None else None,
        energy_performance_preference=_read(f"{SYS}/devices/system/cpu/cpu{probe}/cpufreq/"
                                            "energy_performance_preference") if probe is not None else None,
        no_turbo=_read(f"{SYS}/devices/system/cpu/intel_pstate/no_turbo"),
        throttle_count=throttle_count(probe) if probe is not None else None,
        busy_on_siblings=[f"cpu{s}: {p}" for s in siblings(cpu) for p in busy_processes_on(s, [os.getpid()])]
        if cpu is not None else [],
        cpu_times={str(c): list(t) for c, t in _cpu_times().items() if c in [cpu, *siblings(cpu)]}
        if cpu is not None else {},
    )


def preflight(cpu: Optional[int], min_mem_gb: float, require_pin: bool = False) -> List[str]:
    """Reasons not to start measuring; empty when the machine is usable.
    `require_pin`: the command compares runs (ab, verify), so on a hybrid CPU
    every run must stay on one kind of core."""
    problems = []
    types = core_types()
    if types and cpu is None and require_pin:
        problems.append("this CPU has performance and efficiency cores and an unpinned run moves between "
                        f"them: pass --cpu with a performance core ({_ranges(types['performance'])})")
    if types and cpu is not None and cpu in types.get("efficiency", ()):
        problems.append(f"CPU {cpu} is an efficiency core: pin to a performance core "
                        f"({_ranges(types['performance'])}), or hardware counters count nothing")

    others = running_emulators([os.getpid()])
    if others:
        problems.append("other emulator processes are running: " + ", ".join(others))
    available = _meminfo().get("MemAvailable", 0) / 2**30
    if available < min_mem_gb:
        problems.append(f"MemAvailable is {available:.1f} GB, below {min_mem_gb:g} GB "
                        "(memory pressure stretched compilation 4.4x before)")
    if cpu is not None and cpu not in os.sched_getaffinity(0):
        problems.append(f"CPU {cpu} is not in this process's affinity mask")
    return problems


def _ranges(cpus: Set[int]) -> str:
    ordered, out = sorted(cpus), []
    for cpu in ordered:
        if out and cpu == out[-1][1] + 1:
            out[-1][1] = cpu
        else:
            out.append([cpu, cpu])
    return ",".join(f"{a}-{b}" if a != b else f"{a}" for a, b in out)
