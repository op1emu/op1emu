# Profiling

Measurement support for the emulator itself: where a boot spends its time,
how much guest work it did, and whether a change made it faster. It is built
only on request and is meant to be driven by the `tools/op1prof` runner
(see [Measuring with op1prof](#measuring-with-op1prof)); the first part of
this page describes what the emulator records.

## Build

```sh
cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=Release -DENABLE_PROFILING=ON
cmake --build build-prof --target op1emu
```

`ENABLE_PROFILING=ON` also turns on bcore's observability API and jitdump
support (`BCORE_ENABLE_PROFILE`, `BCORE_PERF_JIT_EVENTS`), frame pointers,
and no tail calls (`-fno-optimize-sibling-calls`): a function that ends by
jumping to another drops out of every call chain, frame-pointer and LBR
alike, and its callee's time is charged to its caller's caller. Before
this, `RTC::UpdateInterrupts` showed 22.8 s inclusive under
`BlackFinCpu::Run` and only 5.4 s under `RTC::ProcessWithInterrupt`, which
ends with it.
With it OFF (the default) none of the hooks below exist in the binary, and the
profiling options are rejected.

Every build records what it was built from: `op1emu --build-info` prints one
line of JSON (`op1.build.v1`) with the host and bcore revisions, whether
either had uncommitted changes to tracked files, the build type, whether
profiling is on, and the compiler. A profiling run prints the same object
as a `[build]` line before its marks and in the trace header (`build`). With a
multi-config generator each configuration gets its own header, so a Debug and
a Release binary from one tree say which they are.

`-DOP1_BUILD_HEADLESS_TESTS=ON` adds the firmware-free tests; with profiling
on they include the mark-firing rule (`op1-mark-set-test`) and a profiling
run on a synthetic LDR (`op1-profile-smoke`: marks, trace, cache flush,
census, and options refused before the CPU starts). CI runs both.

Measure with `--deterministic` (see the README): by default the guest is
paced to the host's clock and keys arrive on host time, so two runs never
execute the same thing.

## Options

| Option | Records |
|---|---|
| `--profile-marks FILE` | Phase marks: one `[mark]` line per mark on stdout |
| `--profile-trace FILE` | Chrome/Perfetto JSON trace of translations, JIT cache flushes and marks |
| `--profile-trace-events N` | The trace recorder's capacity (default 262144) |
| `--profile-census PREFIX` | Per-block, block-to-block and MMIO counts at every mark (diagnostic) |
| `--profile-until MARK` | Stop cleanly when MARK fires |
| `--profile-end-cycles N` | A mark `end` at the first block boundary at or past N guest cycles: a phase end the firmware has no PC for (`--profile-until end`) |
| `--perf-ctl-fifo FIFO` | Control FIFO of `perf record --control=fifo:FIFO` |
| `--perf-window A:B` | Write `enable` to the FIFO at mark A and `disable` at mark B |
| `--perf-jitdump` | LLVM jitdump for `perf inject --jit` (under `$JITDUMPDIR/.debug/jit/`) |
| `--profile-counters` | The CPU thread's user-space instructions and cycles at every mark and translation |
| `--profile-dump-ir PCS` | The LLVM IR of the blocks at these PCs (comma-separated hex), lifted and optimized, into `--profile-dump-dir` (default `ir`) |
| `--profile-probes` | Capture the marks file's probes: registers and memory at chosen block entries (diagnostic) |
| `--profile-checksum FILE` | Guest-state checksums every `--profile-checksum-every N` blocks (default 65536), from block `--profile-checksum-from` (diagnostic) |

Every option is checked before the CPU starts: an unknown mark, a missing FIFO
or a marks file for another NAND image is an error, not an empty profile.

## Phase marks

A marks file names guest PC ranges in the firmware. Every mark fires at a
block entry, so the counters and censuses it closes end on a block boundary
(the verified frame completes during device processing, and its mark fires
at the entry of the next block). A mark fires the first
time a block starts inside its range after starting outside it; `after` arms a
mark only once another mark has fired (and the PC must still enter the range
afresh). At most one of the file's marks fires per block, in file order; the
frame mark, which waits for the next block after the frame completes, can
fire on the same block just before one of them. List marks in the
order they fire (the frame mark counts as last): `op1prof perf` checks its
window against that order before a capture and verifies it after.
`profiling/marks/op1-stock-nand.json` covers the stock image:

| Mark | Meaning |
|---|---|
| `bootrom`, `te-boot` | first and second entry to the L1 bootrom range |
| `lap-begin`, `main-boot`, `lap-end` | firmware boot stages |
| `register_cb`, `main-display` | callback registration, then the main display routine |
| `main-frame` | the first complete panel frame after `main-display` whose RGB565 bytes have the reference SHA-256; it fires at the next block entry, as every mark does |

`main-display` is a firmware address, not proof that anything was drawn; an
earlier, complete but different frame (a stale boot logo transfer) is rejected
by `main-frame`. Every marks file must carry the SHA-256 of its NAND image
(`nand_sha256`, lowercase hex), and the emulator refuses a file without one or
with any other image: on other firmware the same addresses mean nothing, and
the marks would simply never fire.

Each mark prints cumulative counters since the `start` mark (the CPU thread's
first block), so any two marks can be differenced:

```
[mark] main-display pc=0x019ab8b6 wall_ms=... cpu_ms=... process_cpu_ms=... runs=... packets=... cycles=... translated=... mmio_r=... mmio_w=... events=... irq=... irq_latched=... [host_insns=... host_cycles=...] deterministic=1
```

- `wall_ms` is monotonic time, `cpu_ms` the CPU thread's own CPU time. The
  difference is time the thread was not running (preemption, swap stalls).
  `process_cpu_ms` covers every thread of the emulator: the excess over
  `cpu_ms` is the frontend, the USB/IP loop and the rest, which can compete
  with the CPU thread for its core.
- `runs` is blocks executed, those a run chained included (see
  [Chaining](#chaining)), `packets` instruction packets (bcore's
  `CpuState::packets`), `cycles` guest cycles (equal to packets in
  deterministic mode), `translated` blocks compiled.
- `mmio_r`/`mmio_w` count guest accesses at or above 0xFFC00000; `irq`
  counts interrupts and exceptions the guest took (bcore's
  `BcoreStats::interrupts_entered`; the trace has them per IVG level).
  `irq_latched` counts the interrupts the host raised that set a new bit in
  the CEC's ILAT; the trace has them per level too.
- `events` counts delivered CPU-queue events (device callbacks, interrupt
  raises). It is how the host does the work, not what the guest sees: a
  device interrupt level that stays asserted is raised again after every
  block until the guest takes it, and those repeats leave ILAT as it was.
  It is reported as shape, beside `runs` and `translated`, which an
  optimization may change. A stock boot to `main-display` delivers 626,738
  events, takes 68,688 interrupts and exceptions, and latches 62,454 new
  ILAT bits from host raises.
- `host_insns`/`host_cycles` (only with `--profile-counters`) are the CPU
  thread's user-space instructions and cycles, read from the CPU's
  performance counters. They need `kernel.perf_event_paranoid` at most 2 and,
  on a hybrid CPU, a CPU thread pinned to a performance core: the emulator
  refuses to start without the counters and fails the run if they counted
  only part of the time (as on an efficiency core).

With `--deterministic` and the same inputs, every counter except the times
and host counters is identical from run to run. A change that alters
`packets`, `cycles`, `mmio_r`, `mmio_w`, `irq` or `irq_latched` changed what
the guest did, not just how fast the host did it.

## Trace

`--profile-trace` writes `op1.trace.v4` JSON that Perfetto and
`chrome://tracing` open directly. Timestamps are absolute `CLOCK_MONOTONIC`
microseconds, the clock `perf record -k mono` uses.

- `translate` spans (category `jit`) cover one block translation each, with
  `args.cpu_ns`, the CPU thread's CPU time inside the span. Nested `lift`,
  `ir-optimize` and `materialize` spans come from bcore.
- `invalidate` spans (category `jit`, with `args.cpu_ns`) cover bcore's cache
  flush at every entry to the boot ROM (0xFFA00000): it destroys the compiled
  code and builds a new JIT. A stock boot flushes twice, before `lap-begin`,
  for 1.2 and 1.6 ms of CPU: negligible there, but JIT work wherever it
  happens, and no longer counted as execution.
- Compile time is the union of `translate` and `invalidate` spans, the JIT's
  work: never add the nested stages to it.
- With tiered compilation (bcore's `Core::set_tiering`, `--tier-threshold`,
  on by default at 30000) every block is compiled at tier 0 first and again
  at tier 1 once it started that many times; `args.tier` says which (absent
  untiered). A promotion compiled on bcore's worker thread
  (`Core::set_tier_background`, on by default; `--no-tier-background`)
  makes two `translate` spans on the CPU thread, `args.part` `lift` (the
  module, no code yet: it counts as compile time, not as a translation) and
  `link` (loading the worker's code, with its metrics), and one
  `translate-bg` span (tid 2, `args.cpu_ns` the worker's CPU, `args.discarded`
  when its code was not used). The worker's time is never the CPU thread's:
  it is reported apart, as `bg_compile_cpu_ms` (the difference of the
  worker thread's CPU clock, recorded at every mark as `jit_worker_cpu_ns`;
  without it, the CPU of its spans in the phase, exact only where each lies
  wholly inside: one that crosses a mark is split by its overlap, an estimate
  (`bg_exact` false, and the phase's `other_threads_cpu_ms` unknown)), and
  `jit_cpu_ms` adds it to `compile_cpu_ms`. In deterministic mode a
  promotion's code is used `--tier-publish-after` packets (default 2^23)
  after it was asked for, the CPU thread waiting for the worker if need be
  (off-CPU time), so that every run does the same; by default as soon as the
  worker is done. `--jit-cpu N` puts the worker on CPU N (`op1prof`
  pins the whole process to `--cpu` otherwise, worker included).
- A tier-0 block whose instructions all have stencils (bcore's
  `Core::set_copy_patch`, on by default; `--no-copy-patch`) is copied
  together from them in microseconds: its `translate` span has
  `args.copy_patch` true, `args.terminator` `copy-patch`, no IR
  (`ir_insns` 0), `object_bytes` its code and `object_hash` one over its
  stencils' code, relocations and hole values (so `diff compile` compares
  such blocks too), and its
  nested `lift` span holds the whole translation. In the code map its
  variant is 1. With `--perf-jitdump` bcore writes these blocks (named
  `bb_0x<pc>` as LLVM's tier-0 blocks are) to a jitdump of its own beside
  LLVM's, in `.debug/jit/bcore-cp-*`, with the code each arena shares:
  `bcore_cp_exit`, the chain exit of every such block, and
  `bcore_cp_jump_<helper>`, its calls' jumps to the helpers. `op1prof
  report` counts both as generated code (the bucket and the `generated
  code (self)` share) in a table of their own, `generated code no block
  owns`, not as host symbols.
- A packet the build has no stencil for (a parallel packet, a 32-bit
  instruction outside the common ones) gets one at run time, built once
  per encoding for the process (bcore's `Core::set_copy_patch_runtime`, on
  by default at -O0; `--no-cp-runtime`, `--cp-runtime-opt N`), so its
  block is made of stencils too. The `translate` span of a block with such
  stencils has `args.cp_runtime_packets` (its packets that have one) and,
  where this translation built some, `args.cp_runtime_built` (also on an
  LLVM block, where a packet could get none); their CPU is in the span's.
  Every mark prints `[cp-runtime] <mark> built=... refused=... hits=...
  batches=... bytes=... lift_ms=... compile_ms=... extract_ms=...`
  (cumulative), and `op1prof compile` reports the blocks of stencils, those
  with run-time ones, the stencils built and the tier-0 blocks LLVM still
  compiled. On the boot to the main display: 8,574 stencils built, 24
  tier-0 blocks left to LLVM, the CPU thread's compile time 15.0 to 6.7 s.
- Marks (category `mark`) are instant events carrying the same counters as the
  `[mark]` line (including `invalidations`, the cache flushes so far), with `args.thread_cpu_ns`, `args.process_cpu_ns`,
  `args.irq_levels` (entries per IVG level, 0 = EMU ... 15 = IVG15) and
  `args.irq_latched_levels` (new ILAT bits per level).
- With `--profile-counters` the header says `"counters": true` and every
  `translate` and `invalidate` span also has `args.insns`, the instructions
  retired inside it.
- Every successful `translate` span describes the code it produced (bcore's
  `BcoreCompileMetrics`): `ir_insns` and `ir_blocks` of the module as compiled
  (after the IR pipeline), `object_bytes` of machine code, and
  `object_hash`, a hex string hashing that code before relocation together
  with its relocations and, transitively, the constant pools, jump tables
  and function-pointer tables they point into with their own relocations.
  `terminator` names where decoding ended the block (bcore's
  `BcoreCompileMetrics::terminator`): `jump`, `call`, `return`,
  `return-from-event`, `jump-indirect`, `call-indirect`, `loop-setup`,
  `raise`, `exception`, `halt`, `illegal`, `unimplemented`, or `packet-cap`
  at the 256-packet block limit. A conditional branch never ends a block.
  It is a static label of the block, not of each run: a run can leave
  earlier, through a taken branch, a hardware loop or an exception. The same code has the same hash in every process, wherever the JIT
  placed it (bcore's fastmem and rawmem paths would embed a host address and
  break that; op1emu uses neither, and its page-table path loads the table
  from `CpuState` rather than embedding it). `collect_ns` is the time spent collecting
  these metrics, included in the span's `cpu_ns`: about 0.09% of compile
  CPU.

Execution CPU between two marks is their `thread_cpu_ns` difference minus the
`cpu_ns` of the `translate` and `invalidate` spans between them. Unlike wall time, it does not
grow when the thread is descheduled. Execution instructions are split the same
way: the marks' `host_insns` difference minus the spans' `insns`.

The recorder holds 262144 events unless `--profile-trace-events` says otherwise
(the header's `capacity`); a trace that overflows reports `dropped` and
makes the emulator exit nonzero. Do not use it. A trace recorded without
`--deterministic` is refused by every op1prof command that reads one. The header also records
`cpu_thread_tid`, the CPU thread's id, to select its perf samples.

## Chaining

A block that ends runs the next one itself, in generated code, while the
host would have nothing to do after it; only then does it return to
`BlackFinCpu::Run()`. At every return of a block, bcore's `cpu->pc` is where
the host would continue, so a chained block is a host round trip without the
host. Before each run the host computes the run's budget in packets
(`BlackFinCpu::ChainBudget`), the earliest guest time at which it would act
after a block: an event, the panel's TE edge, a DMA channel's next element
(memory DMA: the next row end the guest is told of; see below),
the next TWI byte, the RTC's next calendar change, the core timer's expiry or
the next GP timer tick. It is 0 when a device it services already has work, and
at PCs where the host must see the block start: 0xFFA00000 (which drops every
translation) and, in profiling builds, the range of every mark not yet fired.
During the run the budget drops to 0 when the host gets work: an access
after which the device it reached may need the host (anything but plain RAM,
unless the device says otherwise; see below), an event a device queues during
the access, anything another thread hands the CPU thread, and in bcore every
interrupt entry, return, exception and halt.

A device answers for its accesses after each one (`Device::HostWorkAfterAccess`;
yes by default). The host's work after a block is the device service, the
timers, the panel, events and latching interrupt lines, and the run's budget
already covers what of it comes due in time, so an access that changes none
of it, nor when any of it comes due, can let the run go on. Levels an access
changes are published inside it, as before. The NFC says no when its service
would then change nothing (it sets NBUSY and RD_RDY from the flash and
publishes its level; it is decided again from the state each access, DMA
transfer and ready callback leaves): the firmware's byte-by-byte PIO reads
stay in generated code. So do reads of the CEC (IPEND), the DMA and the GP
timers, and acknowledgements of a DMA channel's IRQ_STATUS.
A block that ends at or past the budget, with an interrupt pending, or whose
successor is not compiled returns. Chaining runs the guest's work block for
block as before: every mark's counters, `runs` and `translated` included, are
identical with `--no-chain`, which turns it off.

Guest time is packets and every due time is a packet count, so that holds
whether or not the run is paced to the host's clock; but only a
`--deterministic` run repeats a run's work, so compare those.

Census, probe and checksum runs count every block start, so they never
chain; `--profile-until` stops chaining once its mark fires. Every other
profiling option works on chained runs.

`--profile-chain-dry` compiles blocks to say they would chain
(`CpuState::chain_would`) and return instead, and the CPU then checks that
it does nothing after them: no device service, timer tick or expiry, TE
edge, event, latch or interrupt check. The NFC and the DMA, whose accesses
keep chaining when their service would do nothing, are then serviced anyway
and must come out of it unchanged (`violation_quiet` when not). A `[chain]` line after every mark
counts the returns to the host by reason, cumulative:

```
[chain] main-frame returns=994324 chained=364262006 host_only=2 service=8567 deadline=238478 access=479438 cec=163251 pending=0 miss=104588 would=0 violations=0
```

`returns` are runs, `chained` the blocks they ran after their first;
`host_only` and `service` runs had a budget of 0 (a host-only PC; a device
with work), `deadline` ended at the budget, `access` after a device access,
`cec` after an interrupt entry or return, an exception, a halt or another
thread, `pending` with an interrupt to check, `miss` at a block not in the
chain table. With the dry run, `would` counts the blocks that would have
chained and `violations` those after which the host did something anyway,
by kind (`violation_service=...`). On a stock boot to `main-frame` (the
ECC-repaired image), 364,262,006 of 365,256,330 block ends would have
chained, with no violation; chained, the boot returns to the host 994,324
times. Before device accesses could keep chaining it returned 11,449,265
times, 10,926,629 of them after a device access, nearly all to an NFC or a
DMA service with nothing to do; now 479,438.

A `[due]` line follows it: the `deadline` returns by what set the budget
(`tick` the GP timers' fixed tick, `coretimer`, `event`, `panel`, `rtc`,
`twi`, `end`, `cap` the 2^24-packet limit, and `dma_<peripheral>` the DMA
channel that came due first), cumulative:

```
[due] end end=1 coretimer=9863 tick=399564 event=5 panel=1000 twi=6091 dma_ppi=80625 dma_sport0tx=380940 dma_mdmad0=98849 dma_mdmas0=4165892
```

That is the synth phase below, start to `end`, as it was: of 5.14M deadline
returns, 4.17M waited for the next row of memory DMA 0's source. Memory DMA
now settles its rows (`DMA::Settle`; `--no-dma-settle` turns it off): each
moves as a service at its own cycle would have moved it, but only when
something could see it -- a row end that interrupts or the transfer's end
(the deadline), a write to the DMA, a read of the channel's current address
or counts, an access to memory the transfer has yet to reach: from the
service after the write that starts it, the pages the rest of the transfer
lies in are watched (`Emulator::WatchPages`, not direct for generated code),
so every load and store, the guest's or the host's, sees memory as at its
own cycle. Every
mark's counters and digests are as before but for `mmio_r` (the poll of the
channel's IRQ_STATUS now waits in one run); of 389,458 deadline returns,
16,145 are memory DMA 0's.

An `[idle]` line follows: the IDLE waits so far (BlackFinCpu::Wait), the
guest cycles they let pass without a packet, and what woke them, by the
same sources. The `[mark]` lines' `cycles` include those cycles, `packets`
do not.

A `[loops]` line follows it: what bcore made of the guest's loops so far,
from its counters (`BcoreStats`). `loop_blocks` are the blocks compiled as
loop blocks (blocks that run themselves again keep the guest registers in
host registers across the loop; `--no-loop-blocks` turns them off),
`proven` their loops given a copy with every guest memory access checked
once at the loop's entry, `speculated` the proven copies of a saturating
MAC sum given a speculative copy, `vectorized` the loops the vectorizer
vectorized, `steady` the loops given the test that skips a waiting
loop to its last iteration (`--steady-skip`, on by default), with
`steady_iterations` the iterations `--steady-skip dry` counts it would have
skipped, and `circular` the circular DAG registers loop blocks took to stay
in their buffers.

`--profile-mark-digest` prints a `[digest]` line at every mark: hashes of the
guest registers (by group), the CEC's IPEND, ILAT and IMASK, and every RAM
device. Two runs that did the same work print the same lines, whatever the
host did between blocks, so it compares a chained run with `--no-chain`
where a checksum run cannot.

## Census

`--profile-census PREFIX` writes `PREFIX.pc.MARK`, `PREFIX.mmio.MARK`,
`PREFIX.mmiofirst.MARK` and `PREFIX.edge.MARK` at every mark, sorted little-endian records with
cumulative counts, and `PREFIX.service.MARK`, `PREFIX.exit.MARK` and `PREFIX.memory.MARK` (below):

- `pc` (`u32 key, u64, u64`): block entry PC, runs, packets executed from that entry.
- `mmio` (`u32 key, u64, u64`): MMR address, guest reads, guest writes.
- `mmiofirst` (`u32 MMR, u32 block PC, u64 first, u64 last`): guest cycles of
  the first and last access to each MMR, and the block of the first.
- `edge` (`u32 from, u32 to, u64 count`): block entry, the entry of the block
  that ran next, how often. The next block is whatever the guest ran next,
  so interrupt entries are edges too. A transition is counted in the phase
  of the block it leaves, like that block's run, so in every phase the
  transitions out of a block equal its runs; `census --edges` checks this
  against the PC census and refuses dumps that disagree.

Difference two marks' files for one phase. The census costs hash-map updates
per block and per MMR access: it changes timing, so never combine it with a
timing measurement. The counts themselves do not depend on it.

### Exit and memory census

A census run returns to the host after every block (it does not chain).
`PREFIX.exit.MARK` says after
which of those runs the host was needed, per block, as little-endian
records (`u32 block, u32 0, u64 mmio, u64 host, u64 both, u64 accesses`):
runs in which the block accessed an MMR (it called into a device), runs
after which the host did something the guest can see (a productive device
call, an event other than a raise, a new ILAT bit, a GP timer tick, or the
CEC entering or leaving an interrupt, all up to the next block's start),
runs with both, and the block's guest memory accesses. The rest of the
block's runs (from the PC census) needed nothing. As with edges, a run
counts in the phase of the block it ends.

`PREFIX.memory.MARK` (JSON, `op1.memory.v1`) counts the guest's loads and
stores by device and width (8, 16, 32 bits), found the way the emulator
finds the device, with `unmapped` for addresses no device maps. Outside
census runs, generated code loads and stores most of them itself, through
the RAM pages of `Emulator::DirectPages()` (bcore's `Memory::page_table()`);
a census run turns that off, so every access is a call into the host and
is counted, and the exit census's per-block accesses are complete. MMRs
(0xFFC00000 and up) are never in the table: every MMR access is a call in
every run, and `mmio_r`/`mmio_w` count them all. bcore reads
a block's instructions through the same interface while it translates the
block; those reads are compilation, once per translation, and the census
leaves them out (a stock boot's 21,631 translations fetched 263,295 times).

On a stock boot between `main-boot` and `main-display`, 338,146,940 of
340,049,495 block runs (99.4%) needed nothing from the host: 1,640,665 had
accessed an MMR, 197,166 were followed by host work the guest could see,
64,724 both. 266M of the runs end in a jump and 99.8% of those needed
nothing; the FIR loop block 0x01a5e27e alone ran 239,186,304 times, needed
the host 27,817 times, and went on to itself 98.4% of the time. The guest
made 831,806,301 loads and stores: 67.9% to SDRAM, 31.0% to L1 data bank A
(mostly 16-bit reads), and the FIR block made 58% of them, two per run.

### Service census

Between blocks (between runs, when blocks chain) the CPU loop services the
devices that have work: one the
block accessed or a transfer touched, one whose guest-time deadline came (an
RTC second, a SPORT sample, a paced DMA row, a TWI byte), and its events that
are due. Interrupt lines are levels: the CPU latches a line that is high and
neither latched nor in service, when it rises and after every block.
`PREFIX.service.MARK` (JSON, `op1.service.v1`, cumulative) counts that work:

- **Sources**: each device in service order (its name, C++ class and base classes), then
  `block` (MMR accesses while a block runs), `events` (queue callbacks) and
  `other` (the rest of the loop). Per source: service `calls` (devices
  only); `published` interrupt levels and the `transitions` among them (a
  level that differs from the pin's last one); events `queued`, apart from
  the interrupt `raises` queued; `work`, the data a device moved (DMA: bytes,
  but the row and transfer ends of memory DMA, which settles its rows,
  reported through `Device::CountServiceWork`); and `productive` calls,
  those that changed a level, queued an event other than a raise, or moved
  data. Devices report what they move or complete through
  `Device::CountServiceWork`: DMA, TWI and USB bytes, an RTC register write
  completing, the NFC's flash turning ready. A device that reports nothing
  there can still do work the census does not see, so `productive` is a
  lower bound, not proof that a call was useless.
- **Pins**: per SIC input, levels published and transitions, and the source
  that published first.
- **IVG**: per level, the `forwarded` latches of a high line, the raises
  delivered to the CEC, the new ILAT bits they `latched`, and the entries
  the guest took.
- **Queue**: `ProcessEvents` calls, how many found events waiting, the
  summed and largest queue length.
- **Other threads**: the frontend's keys and sensors queue events from their
  own threads; the USB/IP loop hands its requests to the USB device, which
  the CPU thread services. Events and levels from other threads are counted
  apart (`other_threads`) and never attributed to a source or pin, whose
  state the CPU thread owns. The CPU owns the counts and frees them with
  itself, never earlier.
- GP timer ticks dropped by the 64-tick catch-up limit (none should be, in
  deterministic mode), and bcore's cache hits, misses and blocks executed.

Before the devices were serviced only when they had work, the loop called
every device after every block; on a stock boot between `main-boot` and
`main-display` (340,049,495 blocks) that cost 49.8 s of 87.2 s of execution
(RTC 24.9 s, DMA 16.6 s, NFC 8.3 s), and the RTC's unchanging level, published
after every block, made the SIC raise every pending line again: 602,100 raises
for 59,107 new ILAT bits.

## Reads and probes

The marks file can also read guest memory. A mark's `read` entries copy words
when it fires, printed as a `[read]` line and recorded in the trace
(category `read`):

```json
{"name": "main-display", ..., "read": [{"name": "fw-stage-table", "addr": "0x01ab3030", "words": 25}]}
```

`probes` capture registers and memory each time a block starts at a PC, with
`--profile-probes` only (checking every block costs time):

```json
{"name": "vdk-switch", "pc": "0xffa09d40", "regs": ["R0", "R1"],
 "mem": [{"name": "r1_prio", "base": "R1", "offset": "0xc"}, {"name": "current", "addr": "0xff802de4"}]}
```

`regs` takes R0-R7, P0-P5, SP, FP, I0-I3, M0-M3, B0-B3, L0-L3, LT0, LT1, LB0, LB1, LC0, LC1, RETS, RETI, RETX, RETN,
USP, SEQSTAT and SYSCFG; `mem` reads `words` 32-bit words (default 1) at an
absolute `addr` or at a `base` register plus `offset`, up to 16 values per
probe. Each hit is a trace event (category `probe`) with the block count,
packets, guest cycles and the values; a probe keeps at most `limit` hits
(default 65536) and reports the rest as dropped. Probes see block entries
only: a PC inside a block never fires, and the emulator says so at exit.
Neither reads nor probes read the MMR space (0xFFC00000 and up), where a
read can change what the guest does: such an address is refused in the
file and skipped at run time. Neither changes the guest's work: a stock
boot with both does the same packets, interrupts and MMIO accesses as one
without.

The stock marks file reads the firmware's own stage table at `main-display`
and declares the three VDK context-switch paths. At each, the L1 word
`current` already holds the thread switched to, so:

```sh
python3 -m op1prof run --binary $B $IN --probes --until main-display
python3 -m op1prof probes op1prof-run-*/run          # what each probe captured (--top values per field)
python3 -m op1prof threads op1prof-run-*/run --probe vdk-switch --probe vdk-switch-irq \
  --probe vdk-start --thread current --name 14914=main --name 1324=fir-fill --perfetto threads.json
```

`threads` splits each phase's guest time between the threads (time in a
thread includes the interrupts taken while it ran) and can write guest-time
thread tracks for Perfetto. On a stock boot, between `main-boot` and
`lap-end` (1,859 ms of guest time) the main thread ran 68% and the
lowest-priority FIR fill thread, which runs only when every other thread is
blocked, 32%.

## IR of chosen blocks

`--profile-dump-ir PC,...` writes, for every translation of a block starting
at one of those PCs, `bb_0x<pc>.<n>.lifted.ll` (the IR as decoded) and, when
the IR pipeline runs, `bb_0x<pc>.<n>.optimized.ll` (the IR as compiled). `n`
counts translations: the cache is flushed when the boot ROM is re-entered,
so a block may be compiled more than once, and its code may have changed
in between. bcore hands over the text; the files are written at the next
block, outside the translation's span. Printing IR is diagnostic: it adds
to those blocks' compile CPU in the trace. `run --dump-pc PC` and
`perf --dump-pc PC` (repeatable) put the files in the run's `ir/`.

`annotate CAPTURE --pc PC` shows `perf annotate` of that block's generated
code from a perf capture (the first `--lines` lines, default 120, of each
part: the block, its careful and promoted versions (`.careful`, `.t1`) and
their `.cold` parts, whichever have samples), beside the
IR files of the same run. It refuses a
non-precise event, which cannot rank instructions, unless
`--allow-imprecise`:

```sh
python3 -m op1prof perf --binary $B $IN --window main-boot:lap-end --until lap-end \
  --event cycles:upp --period 4000000 --dump-pc 1a5e27e
python3 -m op1prof annotate op1prof-perf-*/capture --pc 1a5e27e
```

## Checksums

`--profile-checksum FILE` appends one 64-byte record every N blocks (a
checksum run returns to the host after every block): the
blocks run so far, packets and guest cycles, the entry PC, the CEC's IPEND,
ILAT and IMASK, and three hashes of the guest registers (data and pointer
registers; accumulators, DAG and loop registers; system registers and ASTAT).
Guest memory is not covered; a difference there reaches the registers soon
after. Two deterministic runs that should do the same work produce the same
records; `op1prof diverge` names the first one that differs. It compares
records by position, so both runs must form the same blocks (the same
`runs` count at each record): use it for one binary with different inputs
or settings, or two binaries that compile the same blocks.

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

Generated code then appears as `bb_0x<pc>` symbols, one per guest block, and
`bb_0x<pc>.cold` for the block's rarely run paths, which bcore's backend
splits off to a region of their own; `op1prof` counts both as the block.
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
python3 -m op1prof verify --binary $B $IN --instrumentation  # no instrumentation may change it
python3 -m op1prof gate   --binary $B $IN --base-bin old/op1emu  # the guest's state: chained, unchained, dry
python3 -m op1prof sweep  --binary $B $IN --param tier-threshold=3000,30000 --rounds 2
python3 -m op1prof ab     --base-bin old/op1emu --new-bin $B $IN --pairs 4
python3 -m op1prof ab     --base-bin $B $IN --aa --pairs 4   # noise floor
python3 -m op1prof extend op1prof-ab-* --pairs 2           # more pairs for the same batch
python3 -m op1prof run    --binary $B $IN --census
python3 -m op1prof census op1prof-run-*/run --phase main-boot:lap-end
python3 -m op1prof compile op1prof-run-*/run --phase main-boot:main-display
python3 -m op1prof census op1prof-run-*/run --phase main-boot:main-display --edges
python3 -m op1prof census op1prof-run-*/run --phase main-boot:main-display --exits   # or --memory
python3 -m op1prof service op1prof-run-*/run --phase main-boot:main-display --capture op1prof-perf-*/capture
python3 -m op1prof diverge op1prof-run-A/run op1prof-run-B/run   # runs made with --checksum N
python3 -m op1prof diff census  op1prof-run-A/run op1prof-run-B/run --phase main-boot:main-display
python3 -m op1prof diff compile op1prof-run-A/run op1prof-run-B/run --phase main-boot:main-display --census op1prof-run-A/run
python3 -m op1prof perf   --binary $B $IN --window main-boot:main-display
python3 -m op1prof report op1prof-perf-*/capture --census op1prof-run-*/run --sort ns-per-run
```

Every run uses `--deterministic`, fixed sensors, `--nand-snapshot`, its own
directory with a copy of the OTP image, and an environment without any `OP1_*`
variable. Extra emulator arguments (`--arg`, `--base-arg`, `--new-arg`) can
vary anything else but are refused if they repeat one of these options or the
marks, trace, counters and `--profile-until` settings, since the emulator
keeps the last value. `ab` also refuses the diagnostic options (census,
probes, checksums, IR dumps, perf control) on either side: they change
timing, and an `ab` measures timing. `--phase` and `--until` are checked
against the marks file before anything runs; `verify` compares every mark
and takes no `--phase`. The binary is copied aside first (by content hash), so a rebuild in
the middle of a batch cannot change what is measured. Runs end at
`--until` (default `main-frame`). A run that exits nonzero, never reaches
that mark, goes `--progress-timeout` seconds without a new mark, or closes its
output without exiting is a failed run; its process group is killed and kept in the results. Each command writes
`results.json` with every run, its command line, the SHA-256 of its NAND, OTP
and marks files, and the machine state before and after it. `verify
--write-expect` saves those hashes and `--expect` compares them: nothing else
binds the OTP image, and a different one changes the boot (with a mostly
empty image the guest re-entered the bootrom early and never reached `lap-begin`).
A run whose output passes 200 MiB is stopped and failed, before a runaway
print can fill the disk. Each run also records the binary's `[build]` line;
`--trace-events N` (on every command that runs the emulator) sizes the
trace for a run longer than a stock boot. `--script FILE` replaces the
default headless input (fixed sensors, then a wait) and is hashed into the
run, and `--end-cycles N` adds the mark `end` for a phase after the boot:
`tools/op1prof/scripts/synth-keys.txt` opens the synth page and plays three
notes three times, paced by frames, and `--script tools/op1prof/scripts/synth-keys.txt
--end-cycles 4000000000 --until end --phase main-frame:end` measures it.

`verify --instrumentation` runs the binary once per instrumentation setting:
marks and trace only, then with the census, the marks file's probes,
checksums, host counters, and without the trace. Every setting must do the
same guest work, block for block, and the report names the one that did
not. Uninitialized memory once made one binary run 1,151,352,850 packets to
`main-display` without a trace and 851 with one: what a diagnostic run
showed was then not what a timing run did. Probes are skipped when the marks
file declares none, counters when `kernel.perf_event_paranoid` is above 2. It refuses
diagnostic options in `--arg`, which would switch one on in every
configuration, the plain one included. On a stock boot to `main-display` all six
settings ran the canonical 346,047,018 blocks and 1,151,352,850 packets,
with 68,688 interrupts taken and 62,454 latched.

`gate` checks a change that must not change what the guest does, on the
boot. The binary runs as it is, with every block returning to the host
(`--no-chain`), with blocks that only count where they would have chained
(`--profile-chain-dry`), and with each `--variant NAME=ARGS` (say
`no-loops=--no-loop-blocks`); `--base-bin` runs an older binary first, and
its run is the reference (else the plain run is). The runs are serial, and
all print the guest's state at every mark (`--profile-mark-digest`:
registers, DAG, system registers, CEC, RAM): every one must have the
reference's state and counted work at every mark, and in every run the
chain counters' `violations` (a block that chained past work that was due)
must be 0. On a stock boot to `main-display` the four runs of one binary,
plain, no-chain, chain-dry and no-loop-blocks, agree, with execution CPU
2.6, 15.4, 35.1 and 3.6 s. `verify` compares the digests too, when its runs
print them (`--arg=--profile-mark-digest`): two runs can count the same
work and hold other registers.

`sweep` runs one binary with several settings, `--rounds` times each
(default 2), serially, the configurations in reverse order in every other
round, so that a machine drifting over the sweep moves none more than
another: every combination of the values of each `--param FLAG=V1,V2,...`
(the flag's dashes optional, as argparse reads `--param --flag=...` as an
option), and each `--config NAME=ARGS`. It reports per configuration the
means of the CPU thread's compile time, the JIT worker's, all JIT CPU,
execution CPU, the CPU thread's wait for the worker in the phase (the
difference of the cumulative `[tier]` `wait_ms` at its two marks), wall
time and, with `--counters`, host instructions; `~` on the worker's and all
JIT CPU where a run's worker CPU is an estimate (no worker clock at the
marks: `bg_exact` in `results.json`); a spread over 5%
between rounds; a configuration whose rounds did different guest work (an
error) and one that did other work than the first configuration (reported:
a setting may change how often the guest polls). It is a survey: between
two settings, `ab`'s paired runs and their interval decide.

Before starting, op1prof refuses to run when another emulator is running (it
would compete for CPU and memory, and a second instance cannot bind the USB/IP
port and spins) or when MemAvailable is below `--min-mem-gb` (default 10).
`--force` overrides; the results still record the machine state.

### `ab`

Pairs alternate AB and BA. Each pair is gated on the guest workload: both
sides must fire the same marks with the same packets, cycles, MMIO accesses,
interrupts taken and ILAT bits latched (`runs`, `translated` and `events` may
differ: an optimization that forms longer blocks, or raises an asserted
interrupt fewer times, changes them legitimately). Each side
must also reproduce its own workload across pairs. The verdict is reached on
**execution CPU** (thread CPU between the phase marks minus translation CPU),
with a 95% interval over whole pairs and a practical threshold `--min-effect`
(percent). The interval is the wider of a bootstrap and Student's t: with
four pairs a bootstrap can never reach past the smallest and largest delta,
so alone it is too narrow. Each metric also reports how many pairs the new
side won and the exact sign-test p value (four of four is p = 0.125, not
evidence by itself). Compile CPU is reported beside it, and with a
worker's promotions all JIT CPU (`jit_cpu_ms`) too. Wall-clock
execution is reported only over clean pairs (both runs: wall at most 1.05x
CPU and at most 1 s off-CPU). The gate compares `irq` and `irq_latched`
only when both sides report them, so a binary older than either can still be
compared on the rest. `extend` and `verify --expect` refuse files of another
schema. `extend` holds a batch's saved arguments to the same instrumentation
rule as `ab`, and the summary re-judges every pair's workload from its runs'
marks, so the pairs an older op1prof ran are gated like the new ones.

With `--counters` (also on `run` and `verify`) every run counts host
instructions, and `ab` reports **execution instructions** with their own
interval beside the CPU result. They barely vary between runs of one
configuration, so they show whether a change altered what the host executes
long before CPU time can resolve it; but they are work, not time (a change
can execute more instructions and still run faster), so they never decide
the status. In an A/A of four pairs on a stock boot (`main-boot` to
`main-display`), execution CPU ranged 79-90 s (95% interval -3.1..+3.7%)
while execution instructions stayed within 20,000 of 1,197,820,170,000
(about 2e-8): the spread in time was the instructions per cycle (2.35 to
2.52), not the work.

The status is `faster` or `slower` (the interval lies beyond the threshold),
`faster (below threshold)` or `slower (below threshold)` (it excludes zero but
not the threshold: a real difference smaller than the one asked for, which a
plain `inconclusive` used to hide), `inconclusive`, or one of:
`workload changed` (the sides did different guest work: no speed verdict
unless `--allow-workload-change`), `unusable` (a configuration did not
repeat itself), or `failed` (a run failed; the batch stopped).

`ab` also shows where a difference lies, beside the verdict, which they never
change:

- **Phases**: the execution and compile CPU of every phase between
  consecutive marks, as the median difference per pair and the mean
  percentage. A change that speeds up one stage and slows another shows
  both.
- **Shape** at the phase end, base against new: blocks executed, packets per
  block, translations, queue events and cache flushes. Blocks executed
  changing for the same guest work means the blocks were formed
  differently.

`ab` prints both sides' revisions from their `[build]` lines and warns when
they differ in build type, profiling or compiler: such a comparison measures
the build settings as well as the change. A profiling build has hooks a
release build lacks, and `ab` measures only profiling builds; never compare
one with the other's numbers.

`ab` also compares what the two sides compiled. Each run's distinct (block,
code hash) pairs are digested; when both sides produced the same digest,
`ab` says that the observed compile fingerprints are identical: if the change
was meant to alter generated code, it did not reach it. That is all it says.
The same code can still run a different number of times or take other
paths, so identical fingerprints attribute no timing difference to anything.
A run with any successful translation whose hash is unknown (or with none at
all) has no fingerprint, and the comparison is left out. A nonzero JIT optimization level once changed nothing for
months while measurements compared identical code. The translation count, IR
instructions and machine-code bytes of each side are printed beside it.

`op1prof extend BATCH --pairs N` adds N (even) pairs to an earlier batch with
its own binary snapshots and settings, continuing the AB/BA order, and
recomputes the summary over all pairs. Every run's NAND, OTP and marks hashes
are checked against the ones the batch started with: a run that read another
image stops the batch. The report counts the extensions:
adding pairs after seeing a verdict is a choice the reader should know about.

Every report also lists what the verdict cannot see:

- **Drift**: with six or more runs per side, compile or execution CPU that
  trends with the run order (a rank correlation that a permutation test puts
  below p = 0.01) is printed with each run's value and MemAvailable. Compile time once climbed from 220 s to
  270 s across a batch and took execution time with it.
- **Contention**: each run records how busy its pinned CPU's SMT sibling was
  during the run (from `/proc/stat`) and the core's thermal throttling
  events. Pairs with a sibling above 10% busy or with throttling are listed:
  their CPU time includes sharing the core. The load is an average over the
  run, so a short burst on the sibling barely shows. When at least four pairs
  were not contended, in equal numbers of AB and BA orders (the earliest of
  each order are kept, so leaving pairs out never turns an order effect into
  a difference), the status is decided on those
  (`noncompile_cpu_ms_uncontended`, `decided_on` in the results) and the
  all-pairs result is reported beside it: contention is measured during each
  run, independently of its result. A run that cannot be pinned to one CPU
  (no `--cpu` and more than one allowed) has no per-CPU readings at all. In one A/A of six pairs, three runs with
  a sibling 19-37% busy ran at 1.98-2.20 instructions per cycle instead of
  about 2.43 and took 100-112 s instead of about 83 s; the interval over all
  pairs was -9.1..+21.8%, over the uncontended ones -4.4..+8.9%.

`ab` and `verify` refuse to start unpinned on a hybrid CPU (performance and
efficiency cores, such as 12th-generation Intel), where an unpinned thread
moves between kinds of core, and every command refuses an efficiency core.
Each run's machine state also records the core type, the energy-performance
preference and whether turbo is enabled.

### `census`, `perf` and `report`

`census` differences the census dumps of a `run --census` between two marks:
blocks by packets executed (with runs and packets per run), and MMRs by
accesses. `census --mmio-first` lists the MMRs first accessed in the phase, in order,
with the guest time and the block of the first access: which devices a phase
starts using, and from where. `census --edges` lists the hottest block-to-block transitions and,
for the busiest blocks, how many successors each has and what share of its
runs went to the most common one: a block that nearly always continues to
the same successor is a candidate to join with it, one with many successors
is not.

`census --exits` splits a phase's block runs by why the host was needed
after them (an MMR, host work the guest could see, both, or neither),
groups them by where decoding ended the block (the trace's `terminator`, a
static label: runs that left earlier, through a taken branch or a hardware
loop, count under it too), and
lists the blocks with the most runs that needed nothing, with their most
common successor: the candidates for running more than one block per host
call. A run the census saw no reason for still has to find its successor,
so this is an upper bound on what staying in generated code saves, not a
measure of it. `census --memory` reports guest loads and stores by device and
width and the blocks that access memory most per run; `report --census`
adds accesses per run and ns per access to its block table.

`service RUNDIR --phase A:B` reports the service census of a `run --census`:
the devices that published levels or did anything, with their calls,
productive calls and what they published; the pins by levels published and
the share that changed anything; the interrupt chain per IVG; the event
queue; and dropped timer ticks. The rest of the devices are named in one
line. `--capture CAPTURE` prices the calls with a perf capture of the same
run in all but timing (the join is refused otherwise, as for `report --census`): each
device's price is the inclusive time of the `ProcessWithInterrupt` it runs,
split by calls among the devices that run the same one, as seconds, ns per
call and ns per productive call. Most devices inherit `Device`'s empty
method rather than define their own: the census records each device's base
classes, and the capture binary's symbol table says which classes define
the method, so a device is priced by the first of its class and bases that
does. One whose method cannot be found is shown as `?`, never as a zero. It needs whole call chains (`--call-graph lbr`).
`--json FILE` writes the report.

### `diff`

`diff census A B --phase BEGIN:END` compares two census runs: total block
runs, packets and MMR accesses; the first mark at which their guest work
differs (both runs in firing order: a mark that fired on one side only is a
difference there too, and a counter one run reports at some marks but not
at others is one, while a counter a binary lacks everywhere is left out); the blocks that gained or lost the most packets, and those that
ran on one side only; the MMRs whose accesses changed; and, when both runs
have them, the service and exit censuses. Blocks that ran a different
number of times for the same packets were formed differently. With
`--phase main-boot:main-display` two runs of one binary, before and after
the fix that left instruction fetches out of the memory census, show the
same work at every mark and differ only in 832,069,596 against 831,806,301
guest memory accesses.

`diff compile A B [--phase BEGIN:END] [--census RUNDIR]` compares, per block,
the versions of its code that could run in the phase: the last translation
before the phase began, then every one inside it, by code hash. A version
is distinct code, not a compilation: compiling the same machine code again
adds none. The same versions in the same order are the same code; one
version on each side that differs is different code; several versions whose
sequences differ are *mixed*, since nothing records how much each version
ran; and a block can be compiled on one side only. IR and machine-code sizes and compile time are
shown for the last version. With a
census run it weighs the blocks by the packets they executed in the phase
and says how much of the work ran code that changed, how much ran unchanged
code, and how much is unknown (mixed blocks). Both phase marks must be in
the traces, the first before the second. Where `ab` says only
whether the compile fingerprints are identical, this says which blocks a
change reached and how much they ran; it says nothing about speed.

`census`, `compile`, `service` and `diff` take `--json FILE`.

`run` and `verify` take `--checksum N` (records every N blocks) and
`--checksum-from RUNS`. `diverge A B` reports the first record where two runs
differ, which fields differ, and the window of blocks the divergence lies
in, with the options to repeat both runs at a finer interval inside it. It
reads the files a record at a time and stops at the first difference, so a
fine-grained rerun with millions of records is fine. `verify --checksum`
compares guest state as a check of its own: runs whose counted work matches
but whose registers differ are reported as differences, with where they
begin. The research line needed a separate event-record tool for this, and
two bugs that dropped guest work looked like 5-6% speedups until found.

`--symbols` (on `census`, `compile`, `diverge` and `report`) names PCs from
`ADDRESS NAME` lines, e.g. exported from Ghidra. Counts are work, not cost.

`perf` runs one boot under `perf record -k mono -g`, sampling only between
the two `--window` marks (an inverted window, or an `--until` before the
window end, is rejected rather than recorded empty, and a capture fails if
its window marks did not both fire or the marks fired out of file order),
then `perf inject --jit`. It checks first that perf may
record the event; it never changes `kernel.perf_event_paranoid` but says what
to set (2 is enough).

perf copies every file it records samples in into its build-id cache,
`~/.debug` unless told otherwise, and those copies outlive the capture: a
boot capture samples the binary snapshot and tens of thousands of
`jitted-*.so` files that `perf inject` writes (26,790 files, 276 MB between
`main-boot` and `lap-end`), and a few rounds of captures had filled 19 GB
and the disk. Every perf command op1prof runs on a capture therefore passes
`perf --buildid-dir CAPTURE/buildid` (perf 6.8 ignores the
`PERF_BUILDID_DIR` variable): the cache lives in the capture, is deleted
with it, and keeps `report` and `annotate` working after the binary
snapshot is gone. Captures from before it still use `~/.debug`; what is
left there from them can be removed with the captures they belonged to.

Every capture records perf's own counts of samples,
lost samples and throttling in `capture.json` (samples from `perf report
--stats`; lost samples and throttling from the records in `perf report -D`,
since `--stats` counts records and one record can stand for hundreds of
lost samples; reading them takes about 17 s for a boot's 200k samples), and
`report` warns when more than 1% of the sample records were lost or perf
throttled the rate: the tables then under-sample those stretches. For older
captures it reads them from `perf.data`.

`perf --captures N` records N runs (`capture00`, ...) of the same
configuration, and `report` with several captures (or a directory holding
them) gives every bucket, block and host symbol as a mean over them with a
95% t interval from three captures on. One configuration's shares moved by
2.5% between capture sets in the research line and a baseline's by 11%,
tenfold what Poisson noise on the sample counts suggests: the interval
comes from the captures, never from sample counts. The captures must be
one run in all but timing (the same check as `report --census`), each
given once (a capture listed twice, or with its parent directory, would
narrow the interval for nothing), and each with execution samples in its
window: a window too short for the sampling period has none, and its rows
would read as zero time rather than unknown.

`diff report A B` compares two sets (each a capture or a directory of them)
of the same window and event, no capture on both sides: per bucket, block
and symbol (the binary snapshot and the jitted code are one DSO name in
every capture, so a function of two binaries is one row), the mean
seconds of each, their difference and, with three or more captures a side,
a 95% Welch interval of it. It says where the time went differently, not
whether the total changed (that is `ab`'s verdict). `ab --perf N` takes N
captures of each side over its phase after the timing pairs, in alternating
order, under `perf/base` and `perf/new`, and prints their `diff report`;
they never enter the status, since sampling changes timing. perf's
permission for `--perf-event` (default `cycles:u` with LBR stacks) is
checked before the timing runs. Every row is a test of its own, so across
hundreds of rows some intervals exclude 0 by chance: in an A/A of three
captures a side (`start` to `main-boot`), 10 of 689 did, and the report
prints how many of its own do. Trust a difference that is large or that a
second set repeats.

`report` (also printed after `perf`) keeps the CPU thread's samples inside
the window, drops those inside `translate` and `invalidate` spans, and weights the rest by
their sample period. It splits the trace's execution CPU in proportion, so
every table is in seconds of execution CPU:

- **Host-path buckets** from `profiling/categories.json`: each sample goes to
  the bucket of the frame nearest its leaf that matches a rule (a device read
  called from generated code is device time; a `memcpy` takes its caller's
  bucket). The buckets and `uncategorized` add up to all execution. A
  device's register map (`RegisterDevice::`) has no rule of its own: the RTC
  reading its own registers when it ticks is RTC time, and a guest read
  of them is a memory callback through the frames above. With the register
  map as a memory callback, and tail calls hiding frames, a stock boot had
  shown 15.8 s of memory callbacks against 7.9 s now, and the RTC 6.4 s of
  its 23.2 s.
- **Guest blocks**: `self` is time in the block's own code, `incl` adds what
  it called (memory callbacks, devices reached through them). `--symbols`
  names blocks; `--pc-range NAME=LO:HI` merges a range, such as a loop that
  spans several blocks, into one row.
- **Host symbols**, self and inclusive; `--callers SYMBOL` breaks a symbol
  down by caller.

Inclusive numbers need whole call chains. The report states how much of the
execution weight has a chain reaching the CPU loop (the `roots` in the
categories file); below 95% it withholds the inclusive and caller tables,
since code without frame pointers breaks the chain and the missing callers
would silently undercount. Frame-pointer chains (`perf --call-graph fp`, the
default) reached the loop for 88.5% of a stock boot's execution: generated
code and system libraries keep no frame pointers. Intel LBR call stacks do
not need them and reached 99.8%; they need a hardware event:

```sh
python3 -m op1prof perf --binary $B $IN --window main-boot:main-display \
  --event cycles:u --period 4000000 --call-graph lbr
```

An LBR call stack starts at the target of the last call, not at the
sampled instruction: where the function called jumped on to another, the
stack still names the first. Chained blocks run each other by such jumps
(musttail), so with LBR stacks `report` takes every sample's leaf from a
second `perf script -G` pass, which prints the sampled IP alone, and
refuses the capture when the two passes do not list the same samples in
the same order. Before this, all of a chained run's time went to its first
block: on a stock boot with chaining, `bb_0x01a9bd30` showed 0.37 s of self
time from 6 ms of its own samples, the rest from the blocks it chained to.

The JIT's samples are tables of their own: the CPU thread's inside
translations, and those of bcore's worker thread (a sample of another
thread whose stack has `CompileWorker::`, inside the window: whole call
chains find it, `--call-graph lbr`), in seconds of
`compile_cpu_s` and of the worker's CPU. Each sample goes to the stage of
the leaf-most frame that names one (selection DAG, FastISel, register
allocation, MC emission, object loading, the lifter, the copy-and-patch
translator, ...; `tools/op1prof/compile_samples.py`), an allocator leaf
counting as `malloc in` its caller's stage, and to the nearest LLVM pass
(a legacy `runOnMachineFunction`, or a new pass manager's `PassModel`).
Capture sets and their diffs carry these tables too, their shares of the
JIT's CPU. A table whose CPU is there but that no sample splits (a window
short for the sampling period, or the worker without LBR) is unknown, not
0 s: the report says so, a set leaves it out, and a diff takes no difference
of it. Where the worker's CPU is an estimate (`worker_exact`), the report,
the set and the diff say so.

A capture of several events (`perf --event '{cycles:u,instructions:u}'`)
is attributed event by event: the first in perf's order (`perf evlist`)
that has samples is the one the time is divided by, and the report adds,
for the top blocks and buckets and for all of execution, every event's
count and its ratio to the first (instructions per cycle, there).

`perf script` is read as it prints, and with LBR both passes in step: a
boot's capture is millions of samples, which as lists once took more
memory than the machine had.

For `cpu-clock` it also compares the sampled CPU
time with the trace's execution CPU. For a non-precise event it says that the
totals are only valid per block and symbol.

`report --census RUNDIR` joins a capture with the census of a separate
`run --census` (the census changes timing, so never the same run) and gives
each guest block its runs, packets, seconds, **ns per run** and ns per packet,
plus the cost per run of everything outside guest blocks (dispatch, devices,
event queue, CPU loop). The census run must be the capture's run in all
but timing, or the join is refused: the same binary and inputs (each run
directory has a `run.json` with them), the same extra emulator arguments
apart from diagnostic ones (an `--rtc-epoch` changes what the guest reads
without changing what it counts; an argument naming a file, such as an LDR,
is compared by the file's SHA-256, taken before the run) and the same input
script, the same
guest work, and the same
number of blocks at every mark, since the census's block counts divide the
samples and the workload gate deliberately ignores them. `--sort ns-per-run` ranks blocks by how
expensive one execution is rather than by total share; those orders leave
out blocks that ran fewer than `--min-runs` times (default 1000), whose few
samples would rank them by chance. These are sampled
estimates: their error comes from repeating captures, not from sample counts.
`--json FILE` writes the whole report for further analysis, and
`--categories FILE` replaces `profiling/categories.json`. A trace that does
not name the CPU thread (`cpu_thread_tid`, from binaries before it) is
refused: every thread's samples would count as the CPU thread's.

`compile RUNDIR [--phase A:B]` reports what a run compiled: translations
and blocks, compile CPU with its median, 95th percentile and maximum per
translation, the blocks translated again (the cache is flushed whenever the
boot ROM is re-entered) and what that cost, the stage proportions, the IR and
machine code produced, and the blocks that cost most to compile with their IR
size and compile time per IR instruction. A tiered run adds a line per tier,
and one for the worker's promotions (their CPU and how many were discarded);
"translated again" leaves promotions out.

`run` prints a table of every phase between consecutive marks (CPU,
compile, execution, off-CPU, translations) and stores it in `results.json`.

## Measurement rules

Each rule comes from a result in the research line that was wrong until the
rule was applied; the tool enforces what it can.

- **Measure deterministic runs.** Paced to the host's clock (the default), a
  run sleeps and takes keys on host time, so two runs compare different things.
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
  from one session; run `ab --aa` to see the noise floor. For where the time
  went, take three or more captures a side (`perf --captures`, `ab --perf`).
- **Separate compile from execution by timestamp.** Only the union of
  `translate` and `invalidate` spans is compile time; never add the nested
  stages.
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
  builds. Nor may they change the work: `verify --instrumentation` checks.
- **Check that a knob reached the code.** An optimization level that "made
  no difference" had never reached executed code. If an A/B shows nothing,
  confirm the two sides compiled differently (translation counts, compile
  CPU, the IR) before concluding anything.
