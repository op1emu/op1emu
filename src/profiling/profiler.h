#pragma once

// Host-side profiling for measurement runs. Built only with
// -DENABLE_PROFILING=ON; every hook in the emulator is behind that macro, so a
// normal build carries no profiling code at all.
//
// What a run records (see docs/profiling.md):
//  - phase marks: firmware PC ranges from a JSON file bound to the NAND image,
//    plus an optional verified-frame mark. Each mark prints one `[mark]` line
//    of cumulative counters on stdout, so any two marks can be differenced.
//  - a Chrome/Perfetto trace of translations (with the CPU thread's CPU time
//    per translation) and of the marks.
//  - optional per-PC and MMIO census dumps at every mark (diagnostic: they
//    cost time, never enable them for timing).
//  - perf control: enable/disable a `perf record --control=fifo` window at two
//    marks, and LLVM jitdump output for `perf inject --jit`.
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
    std::string perfFifo;      // --perf-ctl-fifo
    std::string perfWindow;    // --perf-window BEGIN:END
    std::string until;         // --profile-until MARK
    bool jitdump = false;      // --perf-jitdump

    bool Enabled() const {
        return !marksPath.empty() || !tracePath.empty() || !censusPrefix.empty() ||
               !perfFifo.empty() || jitdump;
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

    // Called by BlackFinCpu::Run around each block.
    void BeforeRun(uint32_t pc);
    void AfterRun(uint32_t pc, uint32_t packets);
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
