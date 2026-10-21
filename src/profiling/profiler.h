#pragma once

// Host-side profiling for measurement runs. Built only with
// -DENABLE_PROFILING=ON; every hook in the emulator is behind that macro, so a
// normal build carries no profiling code at all.
//
// What a run records (see docs/profiling.md):
//  - phase marks: firmware PC ranges from a JSON file bound to the NAND image,
//    plus an optional verified-frame mark. Each mark prints one `[mark]` line
//    of cumulative counters on stdout, so any two marks can be differenced.
//  - a Chrome/Perfetto trace of translations and JIT cache flushes (with the
//    CPU thread's CPU time in each) and of the marks.
//  - optional per-PC, block-to-block and MMIO census dumps at every mark
//    (diagnostic: they cost time, never enable them for timing).
//  - optional guest-state checksums every N blocks, to find where two runs
//    that should do the same work first differ.
//  - guest memory read at marks, and optional probes capturing registers and
//    memory at chosen block entries (see probes.h).
//  - the LLVM IR of chosen blocks, lifted and optimized, as files.
//  - perf control: enable/disable a `perf record --control=fifo` window at two
//    marks, and LLVM jitdump output for `perf inject --jit`.
//  - optional hardware counters: the CPU thread's user-space instructions and
//    cycles at every mark and inside every translation.
//
// Threading: everything except the constructor and Finish() runs on the CPU
// thread. Finish() runs after that thread has been joined.

#include "peripheral/display.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class BlackFinCpu;

struct ProfilerOptions {
    std::string nandPath;      // the image the marks file must be bound to
    std::string marksPath;     // --profile-marks
    std::string tracePath;     // --profile-trace
    std::string censusPrefix;  // --profile-census
    std::string checksumPath;  // --profile-checksum
    uint64_t checksumEvery = 65536;  // --profile-checksum-every (runs)
    uint64_t checksumFrom = 0;       // --profile-checksum-from (first run recorded)
    std::string perfFifo;      // --perf-ctl-fifo
    std::string perfWindow;    // --perf-window BEGIN:END
    std::string until;         // --profile-until MARK
    uint64_t endCycles = 0;    // --profile-end-cycles: mark "end" at this guest cycle (0: none)
    bool jitdump = false;      // --perf-jitdump
    bool counters = false;     // --profile-counters
    bool probes = false;       // --profile-probes: the marks file's probes
    bool markDigest = false;   // --profile-mark-digest: guest state hashes at every mark
    std::vector<uint32_t> dumpIrPcs;  // --profile-dump-ir PC[,PC...]
    std::string dumpIrDir = "ir";     // --profile-dump-dir
    size_t traceEvents = 262144;      // --profile-trace-events: the recorder's capacity
    std::string buildInfo;            // the binary's build identity (JSON), for the trace

    bool Enabled() const {
        return !marksPath.empty() || !tracePath.empty() || !censusPrefix.empty() ||
               !perfFifo.empty() || jitdump || counters || !checksumPath.empty() || probes || !dumpIrPcs.empty() ||
               markDigest || endCycles;
    }
};

class Profiler {
public:
    // Validates the options and loads the marks file; throws std::runtime_error
    // with a message for the user on any problem. requestStop is called on the
    // CPU thread when the --profile-until mark fires.
    Profiler(const ProfilerOptions& options, BlackFinCpu& cpu, std::function<void()> requestStop);
    ~Profiler();
    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    // A run from pc may run blocks after its first without the host seeing
    // their starts (BlackFinCpu::SetChaining): not while the census, probes
    // or checksums count blocks, not once --profile-until fired, and not
    // from a host-only PC.
    bool ChainingAllowed(uint32_t pc) const;
    // The guest cycle at which the "end" mark fires, while it has not: runs
    // stop chaining there, so the mark lands on the same packet however the
    // run is split into host returns. 0 once fired or without one.
    uint64_t EndCycles() const;
    // Blocks at pc must start with the host: one may fire a mark.
    bool HostOnly(uint32_t pc) const;

    // Called by BlackFinCpu::Run around each run.
    void BeforeRun(uint32_t pc);
    // A run from pc: packets retired and blocks run after the first (chained).
    // In census runs a run is one block.
    void AfterRun(uint32_t pc, uint32_t packets, uint32_t chained);
    // Called around bcore's cache flush at a boot ROM entry: it rebuilds the
    // JIT, which is compile-side cost, not execution.
    void BeforeInvalidate();
    void AfterInvalidate();
    // Called for every guest access to the MMR space (0xFFC00000 and up).
    void OnMmio(uint32_t addr, bool write);
    // Called by FrameTap with each complete frame (RGB565, scan order).
    void OnFrame(const std::vector<uint16_t>& pixels);
    bool WantsFrames() const;

    // Writes the trace and reports anything the capture lost. Returns false
    // if an output could not be written or the trace overflowed.
    bool Finish();

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// Display decorator that hands complete frames to the profiler on the CPU
// thread (the PPI DMA runs there) and forwards everything to the frontend.
class FrameTap : public Display {
public:
    FrameTap(std::shared_ptr<Display> inner, Profiler& profiler)
        : inner_(std::move(inner)), profiler_(profiler) {}
    void Initialize(int width, int height) override;
    void UpdateRowBuffer(int x, int y, const void* data, int length) override;
    void SetOnFrameStartCallback(const std::function<void(Display&)>& callback) override {
        inner_->SetOnFrameStartCallback(callback);
    }

private:
    std::shared_ptr<Display> inner_;
    Profiler& profiler_;
    int width_ = 0, height_ = 0;
    std::vector<uint16_t> work_;
    std::vector<uint8_t> seen_;
    size_t remaining_ = 0;
    bool started_ = false;
};
