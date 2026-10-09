#!/usr/bin/env python3
"""Stand-in for a profiling op1emu, for the op1prof tests.

Prints [mark] lines and writes an op1.trace.v2 trace like the real emulator.
Behavior comes from FAKE_* variables (the runner keeps those):
  FAKE_SCALE   multiply execution CPU by this (default 1.0)
  FAKE_WORK    packets per mark step (default 1000)
  FAKE_MODE    comma list: hang, fail, noise (random workload), dropped
  FAKE_ENVLOG  write the received environment here
"""
import json
import os
import random
import sys
import time

args = sys.argv[1:]
def opt(name):
    return args[args.index(name) + 1] if name in args else None

if os.environ.get("FAKE_ENVLOG"):
    with open(os.environ["FAKE_ENVLOG"], "w") as f:
        json.dump(dict(os.environ), f)

mode = set(filter(None, os.environ.get("FAKE_MODE", "").split(",")))
scale = float(os.environ.get("FAKE_SCALE", "1.0"))
work = int(os.environ.get("FAKE_WORK", "1000"))
if "noise" in mode:
    work += random.randint(1, 50)
until = opt("--profile-until")
names = ["start", "bootrom", "main-display", "main-frame"]
events, wall, cpu = [], 0, 0
t0 = 10_000_000_000  # ns, an arbitrary monotonic origin
for step, name in enumerate(names):
    if step:
        # one translation, then execution
        events.append({"name": "translate", "cat": "jit", "ph": "X", "pid": 1, "tid": 1,
                       "ts": (t0 + wall) / 1e3, "dur": 2e6 / 1e3, "args": {"pc": step, "ok": True, "cpu_ns": 2_000_000}})
        wall += 2_000_000
        cpu += 2_000_000
        execution = int(10_000_000 * scale)
        wall += execution
        cpu += execution
    counters = {"runs": step * work // 3, "packets": step * work, "cycles": step * work,
                "translated": step, "mmio_r": step * 7, "mmio_w": step * 5, "events": step * 2}
    events.append({"name": name, "cat": "mark", "ph": "i", "s": "g", "pid": 1, "tid": 1,
                   "ts": (t0 + wall) / 1e3, "args": {"pc": step, "wall_ns": wall, "thread_cpu_ns": cpu, **counters}})
    print(f"[mark] {name} pc=0x{step:08x} wall_ms={wall / 1e6:.3f} cpu_ms={cpu / 1e6:.3f} "
          + " ".join(f"{k}={v}" for k, v in counters.items()) + " deterministic=1", flush=True)
    if name == until:
        break
    if "hang" in mode and step == 1:
        time.sleep(3600)
if opt("--profile-trace"):
    with open(opt("--profile-trace"), "w") as f:
        json.dump({"schema": "op1.trace.v2", "dropped": 1 if "dropped" in mode else 0, "traceEvents": events}, f)
sys.exit(3 if "fail" in mode else 0)
