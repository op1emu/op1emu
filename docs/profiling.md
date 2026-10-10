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
afresh). At most one mark fires per block, in file order.
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
perf record -k mono -g --delay=-1 --control=fifo:/tmp/perf.ctl -o boot.perf -- \
  ./build-prof/op1emu nand.bin --nand-snapshot --headless --deterministic \
  --profile-marks profiling/marks/op1-stock-nand.json --profile-until main-frame \
  --perf-ctl-fifo /tmp/perf.ctl --perf-window main-boot:main-display --perf-jitdump
JITDUMPDIR=. perf inject --jit -i boot.perf -o boot.jit.perf
```

Generated code then appears as `bb_0x<pc>` symbols, one per guest block.
`cpu-clock` samples are fine for symbol and address-range totals but cannot
blame a single instruction; use a precise event such as `cycles:upp` for that.
Unprivileged perf needs `kernel.perf_event_paranoid` at most 2, which allows
user-space events of your own processes (hence the `:u` modifiers); Ubuntu
defaults to 4, which allows none.
