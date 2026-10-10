# Profiling

Measurement support for the emulator itself: where a boot spends its time,
how much guest work it did, and whether a change made it faster. It is built
only on request and is meant to be driven by the `tools/op1prof` runner (added
separately); this page describes what the emulator records.

## Build

```sh
cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=Release -DENABLE_PROFILING=ON
cmake --build build-prof --target op1emu
```

`ENABLE_PROFILING=ON` also turns on bcore's observability API and jitdump
support (`BCORE_ENABLE_PROFILE`, `BCORE_PERF_JIT_EVENTS`) and frame pointers.
With it OFF (the default) none of the hooks below exist in the binary, and the
profiling options are rejected.

Measure with `--deterministic` (see the README): with host time, a slower
host makes the guest do more work, so two runs never execute the same thing.

## Options

| Option | Records |
|---|---|
| `--profile-marks FILE` | Phase marks: one `[mark]` line per mark on stdout |
| `--profile-trace FILE` | Chrome/Perfetto JSON trace of translations and marks |
| `--profile-census PREFIX` | Per-PC and MMIO counts at every mark (diagnostic) |
| `--profile-until MARK` | Stop cleanly when MARK fires |
| `--perf-ctl-fifo FIFO` | Control FIFO of `perf record --control=fifo:FIFO` |
| `--perf-window A:B` | Write `enable` to the FIFO at mark A and `disable` at mark B |
| `--perf-jitdump` | LLVM jitdump for `perf inject --jit` (under `$JITDUMPDIR/.debug/jit/`) |

Every option is checked before the CPU starts: an unknown mark, a missing FIFO
or a marks file for another NAND image is an error, not an empty profile.

## Phase marks

A marks file names guest PC ranges in the firmware. A mark fires the first
time a block starts inside its range after starting outside it; `after` arms a
mark only once another mark has fired (and the PC must still enter the range
afresh). At most one mark fires per block, in file order. List marks in the
order they fire (the frame mark counts as last): `op1prof perf` checks its
window against that order before a capture and verifies it after.
`profiling/marks/op1-stock-nand.json` covers the stock image:

| Mark | Meaning |
|---|---|
| `bootrom`, `te-boot` | first and second entry to the L1 bootrom range |
| `lap-begin`, `main-boot`, `lap-end` | firmware boot stages |
| `register_cb`, `main-display` | callback registration, then the main display routine |
| `main-frame` | the first complete panel frame after `main-display` whose RGB565 bytes have the reference SHA-256 |

`main-display` is a firmware address, not proof that anything was drawn; an
earlier, complete but different frame (a stale boot logo transfer) is rejected
by `main-frame`. Every marks file must carry the SHA-256 of its NAND image
(`nand_sha256`, lowercase hex), and the emulator refuses a file without one or
with any other image: on other firmware the same addresses mean nothing, and
the marks would simply never fire.

Each mark prints cumulative counters since the `start` mark (the CPU thread's
first block), so any two marks can be differenced:

```
[mark] main-display pc=0x019ab8b6 wall_ms=... cpu_ms=... runs=... packets=... cycles=... translated=... mmio_r=... mmio_w=... events=... deterministic=1
```

- `wall_ms` is monotonic time, `cpu_ms` the CPU thread's own CPU time. The
  difference is time the thread was not running (preemption, swap stalls).
- `runs` is blocks executed, `packets` instruction packets (bcore's
  `CpuState::packets`), `cycles` guest cycles (equal to packets in
  deterministic mode), `translated` blocks compiled.
- `mmio_r`/`mmio_w` count guest accesses at or above 0xFFC00000; `events`
  counts delivered CPU-queue events (device callbacks, interrupts).

With `--deterministic` and the same inputs, every counter except `wall_ms` and
`cpu_ms` is identical from run to run. A change that alters them changed what
the guest did, not just how fast the host did it.

## Trace

`--profile-trace` writes `op1.trace.v2` JSON that Perfetto and
`chrome://tracing` open directly. Timestamps are absolute `CLOCK_MONOTONIC`
microseconds, the clock `perf record -k mono` uses.

- `translate` spans (category `jit`) cover one block translation each, with
  `args.cpu_ns`, the CPU thread's CPU time inside the span. Nested `lift`,
  `ir-optimize` and `materialize` spans come from bcore. Only the union of
  `translate` spans is compile time: never add the nested stages to it.
- Marks (category `mark`) are instant events carrying the same counters as the
  `[mark]` line, with `args.thread_cpu_ns`.

Execution CPU between two marks is their `thread_cpu_ns` difference minus the
`cpu_ns` of the `translate` spans between them. Unlike wall time, it does not
grow when the thread is descheduled.

The recorder holds 262144 events; a trace that overflows reports `dropped` and
makes the emulator exit nonzero. Do not use it. The header also records
`cpu_thread_tid`, the CPU thread's id, to select its perf samples.

## Census

`--profile-census PREFIX` writes `PREFIX.pc.MARK` and `PREFIX.mmio.MARK` at
every mark: little-endian records of `u32 key, u64, u64`, sorted by key, with
cumulative counts.

- `pc`: block entry PC, runs, packets executed from that entry.
- `mmio`: MMR address, guest reads, guest writes.

Difference two marks' files for one phase. The census costs a hash-map update
per block and per MMR access: it changes timing, so never combine it with a
timing measurement. The counts themselves do not depend on it.

## perf

```sh
mkfifo /tmp/perf.ctl
JITDUMPDIR=. perf record -k mono -g --delay=-1 --control=fifo:/tmp/perf.ctl -o boot.perf -- \
  ./build-prof/op1emu nand.bin --nand-snapshot --headless --deterministic \
  --profile-marks profiling/marks/op1-stock-nand.json --profile-until main-frame \
  --perf-ctl-fifo /tmp/perf.ctl --perf-window main-boot:main-display --perf-jitdump
perf inject --jit -i boot.perf -o boot.jit.perf
```

`JITDUMPDIR` tells the emulator where to write the dump (default `$HOME`);
`perf inject` needs no setting, as it opens the dump through the path perf
recorded when the emulator mapped it.

Generated code then appears as `bb_0x<pc>` symbols, one per guest block.
`cpu-clock` samples are fine for symbol and address-range totals but cannot
blame a single instruction; use a precise event such as `cycles:upp` for that.
Unprivileged perf needs `kernel.perf_event_paranoid` at most 2, which allows
user-space events of your own processes (hence the `:u` modifiers); Ubuntu
defaults to 4, which allows none.

## Measuring with op1prof

`tools/op1prof` drives profiling builds. It needs only Python 3 and, for
`perf`, Linux perf. Run it from `tools/`:

```sh
cd tools
B=../build-prof/op1emu; IN="--nand ../nand.bin --otp ../otp.bin --cpu 2"
python3 -m op1prof run    --binary $B $IN                    # one boot: marks, compile/execution split
python3 -m op1prof verify --binary $B $IN --runs 2           # the workload must repeat exactly
python3 -m op1prof ab     --base-bin old/op1emu --new-bin $B $IN --pairs 4
python3 -m op1prof ab     --base-bin $B $IN --aa --pairs 4   # noise floor
python3 -m op1prof run    --binary $B $IN --census
python3 -m op1prof census op1prof-run-*/run --phase main-boot:lap-end
python3 -m op1prof perf   --binary $B $IN --window main-boot:main-display
python3 -m op1prof report op1prof-perf-*/capture
```

Every run uses `--deterministic`, fixed sensors, `--nand-snapshot`, its own
directory with a copy of the OTP image, and an environment without any `OP1_*`
variable. Extra emulator arguments (`--arg`, `--base-arg`, `--new-arg`) can
vary anything else but are refused if they repeat one of these options or the
marks, trace and `--profile-until` settings, since the emulator keeps the last
value. The binary is copied aside first (by content hash), so a rebuild in
the middle of a batch cannot change what is measured. Runs end at
`--until` (default `main-frame`). A run that exits nonzero, never reaches
that mark, goes `--progress-timeout` seconds without a new mark, or closes its
output without exiting is a failed run; its process group is killed and kept in the results. Each command writes
`results.json` with every run, its command line and the machine state before
and after it.

Before starting, op1prof refuses to run when another emulator is running (it
would compete for CPU and memory, and a second instance cannot bind the USB/IP
port and spins) or when MemAvailable is below `--min-mem-gb` (default 10).
`--force` overrides; the results still record the machine state.

### `ab`

Pairs alternate AB and BA. Each pair is gated on the guest workload: both
sides must fire the same marks with the same packets, cycles, MMIO accesses
and delivered events (`runs` and `translated` may differ, since an
optimization that forms longer blocks changes them legitimately). Each side
must also reproduce its own workload across pairs. The verdict is reached on
**execution CPU** (thread CPU between the phase marks minus translation CPU),
with a 95% bootstrap interval over whole pairs and a practical threshold
`--min-effect` (percent). Compile CPU is reported beside it. Wall-clock
execution is reported only over clean pairs (both runs: wall at most 1.05x
CPU and at most 1 s off-CPU).

The status is `faster`, `slower` or `inconclusive`, or one of:
`workload changed` (the sides did different guest work: no speed verdict
unless `--allow-workload-change`), `unusable` (a configuration did not
repeat itself), or `failed` (a run failed; the batch stopped).

### `census`, `perf` and `report`

`census` differences the census dumps of a `run --census` between two marks:
blocks by packets executed (with runs and packets per run), and MMRs by
accesses. `--symbols` names PCs from `ADDRESS NAME` lines, e.g. exported from
Ghidra. Counts are work, not cost.

`perf` runs one boot under `perf record -k mono -g`, sampling only between
the two `--window` marks (an inverted window, or an `--until` before the
window end, is rejected rather than recorded empty, and a capture fails if
its window marks did not both fire or the marks fired out of file order),
then `perf inject --jit`. It checks first that perf may
record the event; it never changes `kernel.perf_event_paranoid` but says what
to set (2 is enough). `report` (also printed after `perf`) keeps the CPU thread's samples
inside the window, drops those inside `translate` spans, and attributes the
rest by guest block (`bb_0x<pc>`) and by host symbol, beside the trace's
execution and compile CPU. For a non-precise event it says that the totals
are only valid per block and symbol.

## Measurement rules

Each rule comes from a result in the research line that was wrong until the
rule was applied; the tool enforces what it can.

- **Measure deterministic runs.** With host time, a slower host makes the guest
  do more work, so two runs compare different things.
- **Gate on the workload, not just the frame.** Two bugs that dropped work
  looked like 6% and 5% speedups and drew the correct frame; only the packet
  count caught them. `ab` refuses a verdict when the workload moved, unless
  `--allow-workload-change` says the change is intended (an optimization
  that removes guest work); the workload difference is then reported beside
  the verdict, which no longer compares equal work.
- **Use thread CPU for time.** Swap reclaim stalled the CPU thread for 3-15 s
  inside runs whose overall wall/CPU ratio still looked clean, producing
  +163% and -37% "effects". `ab` decides on CPU and reports wall time only on
  clean pairs.
- **Watch the machine.** Memory pressure stretched LLVM compilation 4.4x. On a
  desktop, systemd-oomd may also kill the whole terminal session under
  pressure; run long batches in their own unit (`systemd-run --user`).
- **Error bars come from paired repeats.** Poisson noise on sample counts
  understated the real spread about tenfold: one configuration varied 2.5%
  between capture sets and the baseline 11%. Compare only alternating pairs
  from one session; run `ab --aa` to see the noise floor.
- **Separate compile from execution by timestamp.** Only the union of
  `translate` spans is compile time; never add the nested stages.
- **Shares are not savings.** Removing ~20% of sampled FIR work did not move
  display latency; removing a quarter of a block's machine code saved 3.7%;
  deleting a third of the flag stores saved nothing. Census counts are work,
  not cost, and a profile share is where samples landed.
- **`cpu-clock` cannot rank instructions.** It is right for symbol and range
  totals; for instruction-level questions use a precise event such as
  `cycles:upp` with a fixed `--period` (it needs a hardware PMU).
- **Diagnostics are not free.** A counter set incremented 75M times per boot
  cost 1.4%, enough to swamp a 2% result. Keep census, extra counters and
  perf out of timing runs; compare timing only between equally instrumented
  builds.
- **Check that a knob reached the code.** An optimization level that "made
  no difference" had never reached executed code. If an A/B shows nothing,
  confirm the two sides compiled differently (translation counts, compile
  CPU, the IR) before concluding anything.
