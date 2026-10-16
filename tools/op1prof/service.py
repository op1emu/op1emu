"""The service census (--profile-census, op1.service.v1): how often the host
services each device around a block, how often that did anything, the
interrupt chain from a device's level to the CEC, and the CPU event queue."""
from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Iterable, Optional, Set


SCHEMA = "op1.service.v1"
SOURCE_FIELDS = ("calls", "productive", "published", "transitions", "queued", "raises", "work")
QUEUE_FIELDS = ("process_calls", "nonempty_calls", "length_sum", "queued_other_threads")
IVG_FIELDS = ("forwarded", "raised", "latched", "entered")


def load(prefix: Path, mark: str) -> dict:
    path = Path(f"{prefix}.service.{mark}")
    if not path.is_file():
        raise ValueError(f"{path}: no service census (recorded by an emulator before it, or the mark did not fire)")
    data = json.loads(path.read_text())
    if data.get("schema") != SCHEMA:
        raise ValueError(f"{path}: not a service census ({SCHEMA})")
    return data


def _minus(after: int, before: int, what: str, begin: str, end: str) -> int:
    if after < before:
        raise ValueError(f"service counts went down for {what}: {begin} is not before {end}")
    return after - before


def phase(prefix: Path, begin: str, end: str) -> dict:
    """Counts between two marks (dumps are cumulative). The queue's longest
    length is the longest up to `end`, not within the phase."""
    a, b = load(prefix, begin), load(prefix, end)
    if [s["name"] for s in a["sources"]] != [s["name"] for s in b["sources"]]:
        raise ValueError("the two dumps list different devices")
    sources = []
    for before, after in zip(a["sources"], b["sources"]):
        sources.append({"name": after["name"], "type": after["type"], "bases": after.get("bases", []),
                        "device": after["device"],
                        **{k: _minus(after[k], before[k], after["name"], begin, end) for k in SOURCE_FIELDS}})
    pins_a = {p["pin"]: p for p in a["pins"]}
    pins = []
    for p in b["pins"]:
        q = pins_a.get(p["pin"], {"published": 0, "transitions": 0})
        row = {"pin": p["pin"], "source": p["source"],
               **{k: _minus(p[k], q[k], f"pin {p['pin']}", begin, end) for k in ("published", "transitions")}}
        if row["published"]:
            pins.append(row)
    ivg = []
    for x, y in zip(a["ivg"], b["ivg"]):
        row = {"level": y["level"], **{k: _minus(y[k], x[k], f"IVG{y['level']}", begin, end) for k in IVG_FIELDS}}
        if any(row[k] for k in IVG_FIELDS):
            ivg.append(row)
    queue = {k: _minus(b["queue"][k], a["queue"][k], f"queue {k}", begin, end) for k in QUEUE_FIELDS}
    queue["length_max_so_far"] = b["queue"]["length_max"]
    other = {k: _minus(b.get("other_threads", {}).get(k, 0), a.get("other_threads", {}).get(k, 0),
                       f"other threads' {k}", begin, end) for k in ("published", "forwarded")}
    return {"schema": "op1prof.service.v1", "phase": [begin, end], "sources": sources, "pins": pins, "ivg": ivg,
            "queue": queue, "other_threads": {**other, "queued": queue["queued_other_threads"]},
            "timer_ticks_dropped": _minus(b["timer_ticks_dropped"], a["timer_ticks_dropped"], "timer ticks",
                                          begin, end),
            "bcore": {k: _minus(b["bcore"][k], a["bcore"][k], k, begin, end) for k in b["bcore"]}}


METHOD = "ProcessWithInterrupt"


def implementing_classes(binary: Path) -> Set[str]:
    """Classes whose ProcessWithInterrupt the binary defines (from its symbol
    table): a device runs the first of its class and bases that does."""
    listing = subprocess.run(["nm", "-C", "--defined-only", str(binary)], capture_output=True, text=True)
    if listing.returncode != 0:
        raise RuntimeError(f"nm {binary} failed: {listing.stderr.strip()[-300:]}")
    return parse_implementing(listing.stdout.splitlines())


def parse_implementing(lines: Iterable[str]) -> Set[str]:
    out = set()
    for line in lines:
        name = line.split(" ", 2)[-1]
        if name.endswith(f"::{METHOD}(int)"):
            out.add(name[:-len(f"::{METHOD}(int)")])
    return out


def implementation(source: dict, implemented: Set[str]) -> Optional[str]:
    for name in [source["type"], *source.get("bases", [])]:
        if name in implemented:
            return name
    return None


def join(data: dict, capture_report: dict, implemented: Set[str]) -> dict:
    """Price each device's service calls with a perf capture of the same
    run in all but timing (report.check_same_run, which the caller applies):
    the inclusive time of the ProcessWithInterrupt it runs (its own or the
    one it inherits, from `implemented`) over its calls and its productive
    calls. Devices that run one method share its time by calls; a device
    whose method cannot be found gets no price, not a zero."""
    if tuple(capture_report["window"]) != tuple(data["phase"]):
        raise ValueError(f"the capture's window {capture_report['window']} is not the phase {data['phase']}")
    if not capture_report.get("inclusive_valid"):
        raise ValueError("pricing service calls needs whole call chains: capture with --call-graph lbr")
    inclusive = capture_report["host_inclusive"]
    for source in data["sources"]:
        if source["device"]:
            source["implementation"] = implementation(source, implemented)
    for source in data["sources"]:
        if not source["device"]:
            continue
        impl = source["implementation"]
        if impl is None:
            source["seconds"] = source["ns_per_call"] = source["ns_per_productive"] = None
            continue
        prefix = f"{impl}::{METHOD}("
        seconds = sum(row["seconds"] for row in inclusive if row["symbol"].startswith(prefix))
        same = [s for s in data["sources"] if s["device"] and s.get("implementation") == impl]
        calls = sum(s["calls"] for s in same)
        share = seconds * source["calls"] / calls if calls else 0.0
        source["seconds"] = share
        source["ns_per_call"] = share * 1e9 / source["calls"] if source["calls"] else None
        source["ns_per_productive"] = share * 1e9 / source["productive"] if source["productive"] else None
    data["priced_by"] = capture_report.get("event")
    return data


def _pct(part: int, whole: int) -> str:
    if not whole:
        return "      -"
    share = part / whole * 100
    # A rare useful call (a few thousand in hundreds of millions) is the
    # point of the table: never round it to zero.
    return f"{share:6.2f}%" if share >= 0.01 or not part else f"{share:6.0e}%"


def report(data: dict, top: int) -> str:
    begin, end = data["phase"]
    priced = "priced_by" in data
    called = [s for s in data["sources"] if s["device"] and s["calls"]]
    # Devices that never published a level nor did anything are summarized,
    # unless a capture gave them time.
    # With a capture, those running an inherited method are priced in the
    # shared line below rather than one row each.
    quiet = [s for s in called if not s["published"] and not s["productive"]
             and (not priced or (s.get("implementation") not in (None, s["type"])))]
    devices = sorted((s for s in called if s not in quiet),
                     key=lambda s: -(s.get("seconds") or 0) if priced else -s["published"])
    lines = [f"service {begin} -> {end}: devices serviced between blocks when they have work (an access, a due "
             "time, a transfer); a call is productive when it changed an interrupt level, queued an event other "
             "than an interrupt raise, or moved data",
             "", f"{'device':18s} {'class':14s} {'calls':>12} {'productive':>10} {'':>8} {'published':>12} {'transitions':>11}"
             f" {'raises':>10} {'work':>12}" + (f" {'seconds':>8} {'ns/call':>8} {'ns/prod':>10}" if priced else "")]
    for s in devices[:top]:
        row = (f"{s['name'][:18]:18s} {s['type'][:14]:14s} {s['calls']:>12,} {s['productive']:>10,}"
               f" {_pct(s['productive'], s['calls']):>8}"
               f" {s['published']:>12,} {s['transitions']:>11,} {s['raises']:>10,} {s['work']:>12,}")
        if priced and s.get("seconds") is None:
            row += f" {'?':>8} {'?':>8} {'?':>10}  (no {METHOD} found for {s['type']})"
        elif priced:
            npc, npp = s.get("ns_per_call"), s.get("ns_per_productive")
            row += (f" {s['seconds']:8.3f} {npc if npc is not None else 0:8.1f}"
                    f" {f'{npp:,.0f}' if npp is not None else '-':>10}")
            if s.get("implementation") != s["type"]:
                row += f"  ({s['implementation']}'s, shared)"
        lines.append(row)
    if quiet:
        lines.append(f"and {len(quiet)} devices called {max(q['calls'] for q in quiet):,} times or fewer that never "
                     "did anything: " + ", ".join(q["name"] for q in quiet))
    if priced:
        shared = {}
        for s in called:
            if s.get("implementation") and s["implementation"] != s["type"]:
                shared.setdefault(s["implementation"], []).append(s)
        for impl, members in shared.items():
            seconds = sum(m["seconds"] for m in members)
            lines.append(f"{len(members)} devices run {impl}::{METHOD}: {seconds:.3f} s together, split by calls")
    others = [s for s in data["sources"] if not s["device"] and (s["published"] or s["queued"] or s["raises"])]
    if others:
        lines += ["", f"{'outside the device loop':33s} {'published':>12} {'transitions':>11} {'queued':>10}"
                  f" {'raises':>10}"]
        for s in others:
            lines.append(f"{s['name']:33s} {s['published']:>12,} {s['transitions']:>11,} {s['queued']:>10,}"
                         f" {s['raises']:>10,}")
    if data["pins"]:
        lines += ["", f"{'SIC pin':>7} {'first published by':18s} {'published':>12} {'transitions':>11}"
                  f" {'useful':>8}"]
        for p in sorted(data["pins"], key=lambda p: -p["published"])[:top]:
            lines.append(f"{p['pin']:>7} {p['source'][:18]:18s} {p['published']:>12,} {p['transitions']:>11,}"
                         f" {_pct(p['transitions'], p['published']):>8}")
    if data["ivg"]:
        lines += ["", "interrupt chain per IVG: latches of a high line (one neither latched nor in service), raises "
                  "delivered to the CEC, new ILAT bits, entries",
                  f"{'IVG':>4} {'forwarded':>12} {'raised':>12} {'latched':>9} {'entered':>9} {'latched/raised':>14}"]
        for r in data["ivg"]:
            lines.append(f"{r['level']:>4} {r['forwarded']:>12,} {r['raised']:>12,} {r['latched']:>9,}"
                         f" {r['entered']:>9,} {_pct(r['latched'], r['raised']):>14}")
    q = data["queue"]
    calls = q["process_calls"]
    lines += ["", f"event queue: {calls:,} ProcessEvents calls, {_pct(q['nonempty_calls'], calls).strip()} with "
              f"events, {q['length_sum'] / calls if calls else 0:.3f} events waiting on average, at most "
              f"{q['length_max_so_far']} up to {end}; {q['queued_other_threads']:,} queued by other threads"]
    o = data.get("other_threads", {})
    if any(o.values()):
        lines.append(f"other threads: {o.get('published', 0):,} levels published, "
                     f"{o.get('forwarded', 0):,} latches, {o.get('queued', 0):,} events queued; "
                     "counted, not attributed to any source or pin")
    b = data["bcore"]
    lines.append(f"bcore: {b['blocks_executed']:,} blocks executed, {b['cache_hits']:,} cache hits, "
                 f"{b['cache_misses']:,} misses")
    if data["timer_ticks_dropped"]:
        lines.append(f"warning: {data['timer_ticks_dropped']:,} GP timer ticks were dropped (more than 64 due after "
                     "one block): the guest lost timer time")
    lines.append("Counts are work, not cost; --capture prices the device calls.")
    return "\n".join(lines)
