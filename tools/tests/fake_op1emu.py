#!/usr/bin/env python3
"""Stand-in for a profiling op1emu, for the op1prof tests.

Prints a [mark] line for "start" and each mark of the --profile-marks file,
in file order and the frame mark last, and writes an op1.trace.v3 trace
like the real emulator (with host instruction counts under --profile-counters).
Behavior comes from FAKE_* variables (the runner keeps those):
  FAKE_SCALE   multiply execution CPU by this (default 1.0)
  FAKE_WORK    packets per mark step (default 1000)
  FAKE_MODE    comma list: hang, fail, noise (random workload), dropped,
               closeout (close stdout after the first marks, keep running),
               spew (print without end after the first mark)
  FAKE_IRQ     interrupts taken per mark step (default 3)
  FAKE_CODE    seed of the compiled code's hashes (default 0); FAKE_NOSHAPE drops them
  FAKE_DIVERGE checksum record from which the data registers differ
  FAKE_SKIP    comma list of marks that never fire
  FAKE_BUILD   JSON merged into the [build] line ("none": print no line)
  FAKE_SKEW    OPTION: one more packet per mark step when OPTION is given
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
build = {"schema": "op1.build.v1", "host": "0123456789ab", "host_dirty": False, "bcore": "ba9876543210",
         "bcore_dirty": False, "build_type": "Release", "profiling": True, "compiler": "GNU 12.3.0"}
if os.environ.get("FAKE_BUILD") != "none":
    build.update(json.loads(os.environ.get("FAKE_BUILD") or "{}"))
    print("[build] " + json.dumps(build), flush=True)
scale = float(os.environ.get("FAKE_SCALE", "1.0"))
work = int(os.environ.get("FAKE_WORK", "1000"))
if "noise" in mode:
    work += random.randint(1, 50)
if os.environ.get("FAKE_SKEW") and os.environ["FAKE_SKEW"] in args:
    work += 1
until = opt("--profile-until")
counted = "--profile-counters" in args
irq = int(os.environ.get("FAKE_IRQ", "3"))
with open(opt("--profile-marks")) as f:
    config = json.load(f)
names = ["start"] + [m["name"] for m in config["marks"]] + ([config["frame"]["name"]] if "frame" in config else [])
if opt("--profile-end-cycles"):
    names.append("end")
skip = set(filter(None, os.environ.get("FAKE_SKIP", "").split(",")))
names = [name for name in names if name not in skip]
events, wall, cpu, insns = [], 0, 0, 0
t0 = 10_000_000_000  # ns, an arbitrary monotonic origin
for step, name in enumerate(names):
    if step:
        # one translation, then execution
        span = {"pc": step, "ok": True, "cpu_ns": 2_000_000, **({"insns": 5_000_000} if counted else {})}
        if not os.environ.get("FAKE_NOSHAPE"):
            code = int(os.environ.get("FAKE_CODE", "0"))
            span.update(ir_insns=100 * step, ir_blocks=step, object_bytes=400 * step,
                        object_hash="0" * 16 if os.environ.get("FAKE_UNKNOWN_HASH")
                        else f"{(step * 7919 + code) & 0xffffffffffffffff:016x}")
        events.append({"name": "translate", "cat": "jit", "ph": "X", "pid": 1, "tid": 1,
                       "ts": (t0 + wall) / 1e3, "dur": 2e6 / 1e3, "args": span})
        wall += 2_000_000
        cpu += 2_000_000
        if step == 1:
            # the JIT cache flush at the first boot ROM entry
            events.append({"name": "invalidate", "cat": "jit", "ph": "X", "pid": 1, "tid": 1,
                           "ts": (t0 + wall) / 1e3, "dur": 1e6 / 1e3,
                           "args": {"pc": 0xffa00000, "ok": True, "cpu_ns": 1_000_000,
                                    **({"insns": 2_000_000} if counted else {})}})
            wall += 1_000_000
            cpu += 1_000_000
            insns += 2_000_000
        execution = int(10_000_000 * scale)
        wall += execution
        cpu += execution
        insns += 5_000_000 + 3 * execution
    counters = {"runs": step * work // 3, "packets": step * work, "cycles": step * work,
                "translated": step, "invalidations": min(step, 1), "mmio_r": step * 7, "mmio_w": step * 5, "events": step * 2, "irq": step * irq,
                "irq_latched": step * irq}
    host = {"host_insns": insns, "host_cycles": 2 * insns} if counted else {}
    events.append({"name": name, "cat": "mark", "ph": "i", "s": "g", "pid": 1, "tid": 1,
                   "ts": (t0 + wall) / 1e3, "args": {"pc": step, "wall_ns": wall, "thread_cpu_ns": cpu,
                                                    "process_cpu_ns": cpu + 1000, **counters, **host}})
    print(f"[mark] {name} pc=0x{step:08x} wall_ms={wall / 1e6:.3f} cpu_ms={cpu / 1e6:.3f} "
          f"process_cpu_ms={(cpu + 1000) / 1e6:.3f} "
          + " ".join(f"{k}={v}" for k, v in {**counters, **host}.items()) + " deterministic=1", flush=True)
    if name == until:
        break
    if "spew" in mode and step == 1:
        while True:
            sys.stdout.write("x" * 65536 + "\n")
    if "hang" in mode and step == 1:
        time.sleep(3600)
    if "closeout" in mode and step == 1:
        sys.stdout.flush()
        os.close(1)
        os.close(2)
        time.sleep(3600)
if opt("--profile-checksum"):
    import struct
    every, start = int(opt("--profile-checksum-every") or 65536), int(opt("--profile-checksum-from") or 0)
    diverge = int(os.environ.get("FAKE_DIVERGE", "-1"))
    with open(opt("--profile-checksum"), "wb") as f:
        total = (len(names) - 1) * work // 3
        for index, runs in enumerate(range(start, total + 1, every)):
            regs = 1000 + index + (1 if 0 <= diverge <= index else 0)
            f.write(struct.pack("<QQQIIIIQQQ", runs, runs * 3, runs * 3, 0x1000 + index, 0x10, 0, 0x1f,
                                regs, 7, 9))
if opt("--profile-trace"):
    with open(opt("--profile-trace"), "w") as f:
        json.dump({"schema": "op1.trace.v4", "dropped": 1 if "dropped" in mode else 0, "counters": counted,
                   "build": None if os.environ.get("FAKE_BUILD") == "none" else build, "deterministic": True,
                   "traceEvents": events}, f)
sys.exit(3 if "fail" in mode else 0)
