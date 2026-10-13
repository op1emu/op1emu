"""Per-PC and MMIO census dumps (--profile-census) and their phase differences."""
from __future__ import annotations

import bisect
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

RECORD = struct.Struct("<IQQ")
EDGE = struct.Struct("<IIQ")   # from block entry, to block entry, count
FIRST = struct.Struct("<IIQQ")  # MMR, first block PC, first and last guest cycles


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


def load_edges(path: Path) -> Dict[Tuple[int, int], int]:
    data = path.read_bytes()
    if len(data) % EDGE.size:
        raise ValueError(f"{path}: truncated edge census ({len(data)} bytes)")
    return {(a, b): count for a, b, count in EDGE.iter_unpack(data)}


def edge_phase(prefix: Path, begin: str, end: str) -> Dict[Tuple[int, int], int]:
    """Block-to-block transitions between two marks (dumps are cumulative)."""
    first, last = (load_edges(Path(f"{prefix}.edge.{mark}")) for mark in (begin, end))
    out = {}
    for key, count in last.items():
        delta = count - first.get(key, 0)
        if delta < 0:
            raise ValueError(f"edge counts went down for 0x{key[0]:08x}->0x{key[1]:08x}: {begin} is not before {end}")
        if delta:
            out[key] = delta
    return out


def edges_against_runs(edges: Dict[Tuple[int, int], int], runs: Dict[int, Tuple[int, int]]) -> List[str]:
    """Each run of a block is followed by exactly one block, and both
    censuses count a run in the phase it started in: per block, transitions
    out of it must equal its runs."""
    out: Dict[int, int] = {}
    for (a, _), count in edges.items():
        out[a] = out.get(a, 0) + count
    return [f"0x{pc:08x}: {out.get(pc, 0)} transitions out, {r} runs"
            for pc, (r, _) in sorted(runs.items()) if out.get(pc, 0) != r] + \
           [f"0x{pc:08x}: {n} transitions out, no runs" for pc, n in sorted(out.items()) if pc not in runs]


def edge_report(prefix: Path, begin: str, end: str, limit: int, symbols: Symbols) -> str:
    """Hot transitions, and for the busiest blocks how predictable the next
    block is: a block whose runs nearly all continue to one successor is a
    candidate to join with it (chaining, longer blocks); a block with many
    successors is not. Transitions include interrupt entries."""
    edges = edge_phase(prefix, begin, end)
    total = sum(edges.values())
    lines = [f"edges {begin} -> {end}: {total:,} transitions between {len({a for a, _ in edges}):,} blocks"]
    mismatch = edges_against_runs(edges, phase(prefix, "pc", begin, end))
    if mismatch:
        raise ValueError("edge census disagrees with the block runs it should match: " + "; ".join(mismatch[:5]))
    lines += ["(every block run in the phase is the start of exactly one transition, checked against the "
              "PC census)", "",
             f"{'from':>10} {'to':>10} {'count':>12} {'share':>6}  from symbol -> to symbol"]
    for (a, b), count in sorted(edges.items(), key=lambda kv: -kv[1])[:limit]:
        lines.append(f"0x{a:08x} 0x{b:08x} {count:>12} {count / total * 100:5.1f}%  "
                     f"{symbols.name(a)} -> {symbols.name(b)}")
    out: Dict[int, Dict[int, int]] = {}
    for (a, b), count in edges.items():
        out.setdefault(a, {})[b] = count
    busiest = sorted(out.items(), key=lambda kv: -sum(kv[1].values()))[:limit]
    lines += ["", f"{'block':>10} {'runs':>12} {'succ':>5} {'top succ':>10} {'top %':>6}  symbol"]
    for a, successors in busiest:
        runs = sum(successors.values())
        top, top_count = max(successors.items(), key=lambda kv: kv[1])
        lines.append(f"0x{a:08x} {runs:>12} {len(successors):>5} 0x{top:08x} {top_count / runs * 100:5.1f}%  "
                     f"{symbols.name(a)}")
    return "\n".join(lines)


def mmio_first_report(prefix: Path, begin: str, end: str, window: Tuple[int, int], limit: int,
                      symbols: "Symbols") -> str:
    """MMRs the guest touched for the first time in a phase, in the order it
    did: which device the phase starts using, and from which block. `window`
    is the phase in guest cycles (from the marks)."""
    data = Path(f"{prefix}.mmiofirst.{end}").read_bytes()
    if len(data) % FIRST.size:
        raise ValueError(f"{prefix}.mmiofirst.{end}: truncated ({len(data)} bytes)")
    counts = phase(prefix, "mmio", begin, end)
    lo, hi = window
    rows = sorted((first, addr, pc) for addr, pc, first, _ in FIRST.iter_unpack(data) if lo <= first < hi)
    lines = [f"MMRs first accessed {begin} -> {end}: {len(rows)} "
             f"(guest time from the start; in deterministic mode, at the accessing block's start)", "",
             f"{'MMR':>10} {'first ms':>10} {'reads':>10} {'writes':>10} {'first block':>11}  symbol"]
    for first, addr, pc in rows[:limit]:
        reads, writes = counts.get(addr, (0, 0))
        lines.append(f"0x{addr:08x} {first / 400e3:10.3f} {reads:>10} {writes:>10} 0x{pc:08x}  {symbols.name(pc)}")
    return "\n".join(lines)


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
