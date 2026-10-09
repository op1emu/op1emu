"""Machine state around a measurement, and the checks made before one.

Every condition here moved a historical measurement: memory pressure
stretched LLVM compilation 4.4x and swap reclaim stalled the CPU thread for
3-15 s; a second emulator time-shared the pinned CPU; a benchmark process left
running made two "batches" overlap.
"""
from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass, asdict
from typing import Dict, List, Optional


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

    def to_json(self) -> dict:
        return asdict(self)


def snapshot(cpu: Optional[int]) -> Snapshot:
    info = _meminfo()
    probe = cpu if cpu is not None else 0
    freq = _read(f"/sys/devices/system/cpu/cpu{probe}/cpufreq/scaling_cur_freq")
    return Snapshot(
        mem_available=info.get("MemAvailable", 0),
        swap_used=info.get("SwapTotal", 0) - info.get("SwapFree", 0),
        memory_pressure_avg10=_pressure(),
        loadavg=_read("/proc/loadavg"),
        governor=_read(f"/sys/devices/system/cpu/cpu{probe}/cpufreq/scaling_governor"),
        cpu_mhz=int(freq) / 1000 if freq and freq.isdigit() else None,
        busy_on_cpu=busy_processes_on(cpu, [os.getpid()]) if cpu is not None else [],
    )


def preflight(cpu: Optional[int], min_mem_gb: float) -> List[str]:
    """Reasons not to start measuring; empty when the machine is usable."""
    problems = []
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
