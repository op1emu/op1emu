"""Per-PC and MMIO census dumps (--profile-census) and their phase differences."""
from __future__ import annotations

import bisect
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

RECORD = struct.Struct("<IQQ")


def load(path: Path) -> Dict[int, Tuple[int, int]]:
    data = path.read_bytes()
    if len(data) % RECORD.size:
        raise ValueError(f"{path}: truncated census ({len(data)} bytes)")
    return {key: (a, b) for key, a, b in RECORD.iter_unpack(data)}


def phase(prefix: Path, kind: str, begin: str, end: str) -> Dict[int, Tuple[int, int]]:
    """Counts accumulated between two marks (dumps are cumulative)."""
    first, last = (load(Path(f"{prefix}.{kind}.{mark}")) for mark in (begin, end))
    out = {}
    for key, (a, b) in last.items():
        a0, b0 = first.get(key, (0, 0))
        if a < a0 or b < b0:
            raise ValueError(f"census counts went down for 0x{key:08x}: {begin} is not before {end}")
        if (a - a0) or (b - b0):
            out[key] = (a - a0, b - b0)
    return out


class Symbols:
    """`ADDRESS NAME` lines (hex address, e.g. exported from Ghidra); a PC is
    named after the nearest symbol at or below it."""

    def __init__(self, path: Optional[Path]):
        self.entries: List[Tuple[int, str]] = []
        if path:
            for line in path.read_text().splitlines():
                parts = line.split(None, 1)
                if len(parts) == 2 and not line.startswith("#"):
                    self.entries.append((int(parts[0], 16), parts[1].strip()))
            self.entries.sort()
        self.keys = [address for address, _ in self.entries]

    def name(self, pc: int) -> str:
        index = bisect.bisect_right(self.keys, pc) - 1
        if index < 0:
            return ""
        address, name = self.entries[index]
        return f"{name}+0x{pc - address:x}" if pc != address else name


@dataclass(frozen=True)
class Row:
    key: int
    first: int    # runs, or MMR reads
    second: int   # packets, or MMR writes
    share: float  # of the phase total, percent


def top(counts: Dict[int, Tuple[int, int]], by: int, limit: int) -> Tuple[List[Row], int]:
    total = sum(value[by] for value in counts.values())
    rows = sorted(counts.items(), key=lambda kv: -kv[1][by])[:limit]
    return [Row(key, a, b, (a, b)[by] / total * 100 if total else 0.0) for key, (a, b) in rows], total


def report(prefix: Path, begin: str, end: str, limit: int, symbols: Symbols) -> str:
    lines = [f"census {begin} -> {end}",
             "Counts are executed work, not cost: a cheap block run often can rank above an",
             "expensive one. Join with a perf report before choosing what to optimize.", ""]
    pcs, packets = top(phase(prefix, "pc", begin, end), 1, limit)
    lines.append(f"{'block':>10} {'runs':>12} {'packets':>13} {'share':>6} {'pk/run':>6}  symbol")
    for row in pcs:
        lines.append(f"0x{row.key:08x} {row.first:>12} {row.second:>13} {row.share:5.1f}% "
                     f"{row.second / row.first if row.first else 0:6.1f}  {symbols.name(row.key)}")
    lines.append(f"{'total':>10} {'':>12} {packets:>13}")
    lines.append("")
    mmio = phase(prefix, "mmio", begin, end)
    rows = sorted(mmio.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:limit]
    lines.append(f"{'MMR':>10} {'reads':>12} {'writes':>12}")
    for key, (reads, writes) in rows:
        lines.append(f"0x{key:08x} {reads:>12} {writes:>12}")
    lines.append(f"{'total':>10} {sum(v[0] for v in mmio.values()):>12} {sum(v[1] for v in mmio.values()):>12}")
    return "\n".join(lines)
