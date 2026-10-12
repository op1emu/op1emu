#!/usr/bin/env python3
"""Stand-in for a profiling op1emu, for the op1prof tests.

Prints [mark] lines and writes an op1.trace.v3 trace like the real emulator
(with host instruction counts under --profile-counters).
Behavior comes from FAKE_* variables (the runner keeps those):
  FAKE_SCALE   multiply execution CPU by this (default 1.0)
  FAKE_WORK    packets per mark step (default 1000)
  FAKE_MODE    comma list: hang, fail, noise (random workload), dropped,
               closeout (close stdout after the first marks, keep running)
  FAKE_IRQ     interrupts taken per mark step (default 3)
  FAKE_CODE    seed of the compiled code's hashes (default 0); FAKE_NOSHAPE drops them
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
counted = "--profile-counters" in args
irq = int(os.environ.get("FAKE_IRQ", "3"))
names = ["start", "bootrom", "main-display", "main-frame"]
events, wall, cpu, insns = [], 0, 0, 0
t0 = 10_000_000_000  # ns, an arbitrary monotonic origin
for step, name in enumerate(names):
    if step:
        # one translation, then execution
        span = {"pc": step, "ok": True, "cpu_ns": 2_000_000, **({"insns": 5_000_000} if counted else {})}
        if not os.environ.get("FAKE_NOSHAPE"):
            code = int(os.environ.get("FAKE_CODE", "0"))
            span.update(ir_insns=100 * step, ir_blocks=step, object_bytes=400 * step,
                        object_hash=f"{(step * 7919 + code) & 0xffffffffffffffff:016x}")
        events.append({"name": "translate", "cat": "jit", "ph": "X", "pid": 1, "tid": 1,
                       "ts": (t0 + wall) / 1e3, "dur": 2e6 / 1e3, "args": span})
        wall += 2_000_000
        cpu += 2_000_000
        execution = int(10_000_000 * scale)
        wall += execution
        cpu += execution
        insns += 5_000_000 + 3 * execution
    counters = {"runs": step * work // 3, "packets": step * work, "cycles": step * work,
                "translated": step, "mmio_r": step * 7, "mmio_w": step * 5, "events": step * 2, "irq": step * irq}
    host = {"host_insns": insns, "host_cycles": 2 * insns} if counted else {}
    events.append({"name": name, "cat": "mark", "ph": "i", "s": "g", "pid": 1, "tid": 1,
                   "ts": (t0 + wall) / 1e3, "args": {"pc": step, "wall_ns": wall, "thread_cpu_ns": cpu,
                                                    "process_cpu_ns": cpu + 1000, **counters, **host}})
    print(f"[mark] {name} pc=0x{step:08x} wall_ms={wall / 1e6:.3f} cpu_ms={cpu / 1e6:.3f} "
          f"process_cpu_ms={(cpu + 1000) / 1e6:.3f} "
          + " ".join(f"{k}={v}" for k, v in {**counters, **host}.items()) + " deterministic=1", flush=True)
    if name == until:
        break
    if "hang" in mode and step == 1:
        time.sleep(3600)
    if "closeout" in mode and step == 1:
        sys.stdout.flush()
        os.close(1)
        os.close(2)
        time.sleep(3600)
if opt("--profile-trace"):
    with open(opt("--profile-trace"), "w") as f:
        json.dump({"schema": "op1.trace.v3", "dropped": 1 if "dropped" in mode else 0, "counters": counted,
                   "traceEvents": events}, f)
sys.exit(3 if "fail" in mode else 0)
