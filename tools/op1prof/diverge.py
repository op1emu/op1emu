"""Guest-state checksums (--profile-checksum) and where two runs first differ."""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from .census import Symbols

# u64 runs, u64 packets, u64 cycles, u32 pc, u32 ipend, u32 ilat, u32 imask,
# u64 hash per register group (see Profiler::Impl::WriteChecksum).
RECORD = struct.Struct("<QQQIIIIQQQ")
FIELDS = ("runs", "packets", "cycles", "pc", "ipend", "ilat", "imask", "data_regs", "dag_acc_loop_regs",
          "system_regs_astat")
FILE = "checksum.bin"


@dataclass(frozen=True)
class Record:
    runs: int
    packets: int
    cycles: int
    pc: int
    ipend: int
    ilat: int
    imask: int
    data_regs: int
    dag_acc_loop_regs: int
    system_regs_astat: int


def load(path: Path) -> List[Record]:
    data = path.read_bytes()
    if len(data) % RECORD.size:
        raise ValueError(f"{path}: truncated checksum file ({len(data)} bytes)")
    return [Record(*values) for values in RECORD.iter_unpack(data)]


def _locate(path: Path) -> Path:
    return path / FILE if path.is_dir() else path


def first_difference(a: List[Record], b: List[Record]) -> Optional[dict]:
    """The first record that differs, and the window of blocks it lies in:
    after the last equal record, at or before this one."""
    for index, (x, y) in enumerate(zip(a, b)):
        if x != y:
            fields = [name for name in FIELDS if getattr(x, name) != getattr(y, name)]
            previous = a[index - 1] if index else None
            return {"index": index, "fields": fields, "a": x, "b": y, "after": previous}
    if len(a) != len(b):
        shorter = min(len(a), len(b))
        return {"index": shorter, "fields": ["length"], "a": a[shorter] if shorter < len(a) else None,
                "b": b[shorter] if shorter < len(b) else None, "after": a[shorter - 1] if shorter else None}
    return None


def report(run_a: Path, run_b: Path, symbols: Symbols) -> str:
    a, b = load(_locate(run_a)), load(_locate(run_b))
    found = first_difference(a, b)
    if found is None:
        return f"no difference in {len(a)} checksums (guest registers and interrupt state at every record)"
    after = found["after"]
    lines = []
    if found["fields"] == ["length"]:
        lines.append(f"identical for {found['index']} checksums, then one run has no more "
                     f"({len(a)} vs {len(b)} records): it ended earlier")
        return "\n".join(lines)
    x, y = found["a"], found["b"]
    lines.append(f"first difference at checksum {found['index']} (run {x.runs:,}): {', '.join(found['fields'])}")
    if after:
        lines.append(f"  equal at run {after.runs:,} (pc 0x{after.pc:08x} {symbols.name(after.pc)}), "
                     f"so the runs diverged in the {x.runs - after.runs:,} blocks after it")
    else:
        lines.append("  already different at the first block")
    for name in found["fields"]:
        va, vb = getattr(x, name), getattr(y, name)
        if name == "pc":
            lines.append(f"  pc: 0x{va:08x} {symbols.name(va)} vs 0x{vb:08x} {symbols.name(vb)}")
        elif name in ("ipend", "ilat", "imask"):
            lines.append(f"  {name}: 0x{va:04x} vs 0x{vb:04x} (bits differing 0x{va ^ vb:04x})")
        elif name in ("runs", "packets", "cycles"):
            lines.append(f"  {name}: {va:,} vs {vb:,} ({vb - va:+,})")
        else:
            lines.append(f"  {name} differ")
    if after:
        every = max(1, (x.runs - after.runs) // 64)
        lines.append(f"narrow it down: rerun both with --checksum {every} --checksum-from {after.runs} "
                     f"--until a mark after it, then diverge again")
    return "\n".join(lines)
