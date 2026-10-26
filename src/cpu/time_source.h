#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// Where guest-visible time comes from: one CCLK cycle per packet the core
// executed, at 400 MHz, plus the cycles it waited in IDLE (Idle()). An issue
// model -- no pipeline, stall, cache or PLL timing -- that does not depend on
// host speed, in one of two modes, fixed for the life of the CPU:
//
//  - Real time (default): the CPU keeps guest time from running ahead of the
//    host's clock (RealtimePacer), so the firmware's animation and audio go
//    at the device's pace. Keys and USB come in on host time, so no two runs
//    do the same work. The RTC starts at the host's calendar time.
//
//  - Deterministic: as fast as the host allows. A boot with the same inputs
//    does the same work on every run, and the RTC starts at a fixed epoch.
//    For measurement and regression runs.
class TimeSource {
public:
    using SystemClock = std::chrono::system_clock;
    static constexpr uint64_t kHz = 400000000;
    // RTC wall time at guest time zero in deterministic mode (2024-01-01 UTC).
    static constexpr int64_t kDefaultEpochSeconds = 1704067200;
    // Latest epoch the RTC can show: its day field is 15 bits, counted here
    // from 1970, so it ends on 2059-09-18.
    static constexpr int64_t kMaxEpochSeconds = 32768LL * 86400 - 1;

    explicit TimeSource(bool deterministic)
        : deterministic_(deterministic),
          epoch_(deterministic ? SystemClock::time_point(std::chrono::seconds(kDefaultEpochSeconds))
                               : std::chrono::time_point_cast<SystemClock::duration>(SystemClock::now())) {}

    bool Deterministic() const { return deterministic_; }

    // Packets the core has executed.
    void Retire(uint64_t packets) { packets_ += packets; }
    uint64_t Packets() const { return packets_; }
    // Cycles the core waited in IDLE, between blocks.
    void Idle(uint64_t cycles) { idle_ += cycles; }
    uint64_t IdleCycles() const { return idle_; }

    // While a block runs, the 1-based index of the packet executing in it
    // (bcore's CpuState::packets), 0 between blocks: a device the block
    // accesses then sees the time of the packet accessing it, not of the
    // block's start. bcore stores the index only where it can be read --
    // before each call into the host and when the block returns
    // (Core::set_lazy_state) -- so it is current in a memory callback, not
    // to another thread looking while the block runs.
    void SetBlockPackets(const uint32_t* index) { blockIndex_ = index; }
    // While a run chains blocks (bcore's CpuState::chain_packets), the
    // packets the blocks before the running one retired; 0 between runs.
    void SetChainedPackets(const uint32_t* chained) { chained_ = chained; }

    uint64_t Cycles() const { return packets_ + idle_ + InBlock(); }
    // 2.5 ns per cycle; overflows only past ~9e9 s of guest time.
    uint64_t Nanoseconds() const { return Cycles() * 5 / 2; }

    // Calendar time for the RTC: the epoch plus guest time. In real-time mode
    // that falls behind the host's calendar by the lag RealtimePacer let off,
    // and drifts from it with the sound card's crystal, which guest time
    // follows (a desktop's: 56 ppm, 5 s a day), as the device's RTC and codec
    // crystals do.
    SystemClock::time_point SystemNow() const {
        return epoch_ + std::chrono::duration_cast<SystemClock::duration>(std::chrono::nanoseconds(Nanoseconds()));
    }
    void SetEpoch(SystemClock::time_point epoch) { epoch_ = epoch; }
    // The first guest time at which SystemNow() reaches t.
    // SystemNow() truncates guest nanoseconds to the clock's ticks (1 us in
    // libc++), so k ticks past the epoch are reached at k ticks in
    // nanoseconds; rounding up covers a clock finer than nanoseconds.
    uint64_t DueNs(SystemClock::time_point t) const {
        return t <= epoch_ ? 0 : uint64_t(std::chrono::ceil<std::chrono::nanoseconds>(t - epoch_).count());
    }
    // The first cycle count at which Nanoseconds() reaches ns: the least c
    // with c * 5 / 2 >= ns. UINT64_MAX for never.
    static uint64_t CyclesAt(uint64_t ns) {
        return ns >= UINT64_MAX / 2 ? UINT64_MAX : (ns * 2 + 4) / 5;
    }

private:
    uint64_t InBlock() const {
        return (chained_ ? *chained_ : 0) + (blockIndex_ && *blockIndex_ ? *blockIndex_ - 1 : 0);
    }

    bool deterministic_;
    SystemClock::time_point epoch_;
    uint64_t packets_ = 0;
    uint64_t idle_ = 0;
    const uint32_t* blockIndex_ = nullptr;
    const uint32_t* chained_ = nullptr;
};

// The panel's TE output as a board oscillator on guest time: a 1 us low pulse
// at 60 Hz.
class PanelOscillator {
public:
    static constexpr uint64_t kPulseNs = 1000;

    // The level TE switches to at guest time nowNs (true = low), if any.
    // Service() changes the level from this guest time on.
    uint64_t DeadlineNs() const { return deadlineNs_; }
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
