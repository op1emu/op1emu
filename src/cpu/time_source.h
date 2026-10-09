#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// Where guest-visible time comes from. One of two models, fixed for the life
// of the CPU:
//
//  - Wall (default): host steady_clock, as the emulator has always run. A
//    slower host makes the guest see more time pass per instruction (more
//    timer interrupts, more polling), so no two boots do the same work. The
//    core timer and SPORT pacing divide by kWallSlowdown to make up for an
//    emulator slower than real time.
//
//  - Deterministic: one CCLK cycle per packet the core executed, at 400 MHz.
//    An issue model -- no pipeline, stall, cache or PLL timing -- but it does
//    not depend on host speed, so a boot with the same inputs does the same
//    work on every run. It runs as fast as the host allows and needs no
//    slowdown. For measurement and regression runs; interactive use stays on
//    wall time until the emulator is faster than real time.
class TimeSource {
public:
    using SystemClock = std::chrono::system_clock;
    using SteadyClock = std::chrono::steady_clock;
    static constexpr uint64_t kHz = 400000000;
    static constexpr unsigned kWallSlowdown = 10;
    // RTC wall time at guest time zero in deterministic mode (2024-01-01 UTC).
    static constexpr int64_t kDefaultEpochSeconds = 1704067200;
    // Latest epoch the RTC can show: its day field is 15 bits, counted here
    // from 1970, so it ends on 2059-09-18.
    static constexpr int64_t kMaxEpochSeconds = 32768LL * 86400 - 1;

    explicit TimeSource(bool deterministic)
        : deterministic_(deterministic), start_(SteadyClock::now()),
          epoch_(std::chrono::seconds(kDefaultEpochSeconds)) {}

    bool Deterministic() const { return deterministic_; }

    // Wall mode: guest time zero is now. The CPU calls this once its devices
    // and JIT are set up, so their construction time is not guest time.
    void StartWallClock() { start_ = SteadyClock::now(); }

    // Packets the core has executed, in both models.
    void Retire(uint64_t packets) { packets_ += packets; }
    uint64_t Packets() const { return packets_; }

    uint64_t Cycles() const {
        if (deterministic_) return packets_;
        return std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - start_).count() * 400;
    }
    uint64_t Nanoseconds() const {
        // 2.5 ns per cycle; overflows only past ~9e9 s of guest time.
        if (deterministic_) return packets_ * 5 / 2;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now() - start_).count();
    }
    // Divisor for device rates tuned against wall time; 1 in deterministic mode.
    unsigned Slowdown() const { return deterministic_ ? 1 : kWallSlowdown; }

    // Calendar time for the RTC: the host's in wall mode; epoch plus guest
    // time in deterministic mode, so RTC reads repeat across runs.
    SystemClock::time_point SystemNow() const {
        if (!deterministic_) return SystemClock::now();
        return epoch_ + std::chrono::duration_cast<SystemClock::duration>(std::chrono::nanoseconds(Nanoseconds()));
    }
    void SetEpoch(SystemClock::time_point epoch) { epoch_ = epoch; }

private:
    bool deterministic_;
    SteadyClock::time_point start_;
    SystemClock::time_point epoch_;
    uint64_t packets_ = 0;
};

// The panel's TE output as a board oscillator on guest time: a 1 us low pulse
// at 60 Hz. Deterministic mode only; in wall mode the frontend's host poll
// drives TE, which ties the guest's display timing to host speed.
class PanelOscillator {
public:
    static constexpr uint64_t kPulseNs = 1000;

    // The level TE switches to at guest time nowNs (true = low), if any.
    std::optional<bool> Service(uint64_t nowNs) {
        if (nowNs < deadlineNs_) return std::nullopt;
        low_ = !low_;
        if (low_) {
            deadlineNs_ += kPulseNs;
        } else {
            ++frame_;
            deadlineNs_ = (frame_ * 1000000000ULL + 59) / 60;
        }
        return low_;
    }

private:
    uint64_t frame_ = 0;
    uint64_t deadlineNs_ = 0;
    bool low_ = false;
};
