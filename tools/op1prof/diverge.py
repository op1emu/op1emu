"""Guest-state checksums (--profile-checksum) and where two runs first differ."""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, List, Optional

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


def records(path: Path) -> Iterator[Record]:
    """One record at a time: a fine-grained rerun writes millions."""
    if path.stat().st_size % RECORD.size:
        raise ValueError(f"{path}: truncated checksum file ({path.stat().st_size} bytes)")
    with open(path, "rb") as file:
        while chunk := file.read(RECORD.size * 4096):
            for values in RECORD.iter_unpack(chunk):
                yield Record(*values)


def load(path: Path) -> List[Record]:
    return list(records(path))


def _locate(path: Path) -> Path:
    return path / FILE if path.is_dir() else path


def first_difference(a: Iterable[Record], b: Iterable[Record]) -> Optional[dict]:
    """The first record that differs, and the window of blocks it lies in:
    after the last equal record, at or before this one. Reads only as far as
    that record; `count` is the records compared when there is none."""
    a, b = iter(a), iter(b)
    previous, index = None, 0
    while True:
        x, y = next(a, None), next(b, None)
        if x is None and y is None:
            return None
        if x is None or y is None:
            return {"index": index, "fields": ["length"], "a": x, "b": y, "after": previous,
                    "ended": "a" if x is None else "b"}
        if x != y:
            fields = [name for name in FIELDS if getattr(x, name) != getattr(y, name)]
            return {"index": index, "fields": fields, "a": x, "b": y, "after": previous}
        previous, index = x, index + 1


def count(path: Path) -> int:
    return path.stat().st_size // RECORD.size


def report(run_a: Path, run_b: Path, symbols: Symbols) -> str:
    path_a, path_b = _locate(run_a), _locate(run_b)
    found = first_difference(records(path_a), records(path_b))
    if found is None:
        return f"no difference in {count(path_a)} checksums (guest registers and interrupt state at every record)"
    after = found["after"]
    lines = []
    if found["fields"] == ["length"]:
        lines.append(f"identical for {found['index']} checksums, then one run has no more "
                     f"({count(path_a)} vs {count(path_b)} records): it ended earlier")
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
