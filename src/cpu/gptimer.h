#pragma once

#include "io.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>

class GPTimerImpl;

enum GPTimerClockType {
    GPTimerClockTypeSCLK = 0,
    GPTimerClockTypeTACLK = 1,
    GPTimerClockTypeTMRCLK = 2,
};

class GPTimer : public RegisterDevice {
public:
    GPTimer(u32 baseAddr);
    ~GPTimer();

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;
    // Reads have no effect: the host ticks the timers at its own times.
    bool HostWorkAfterAccess(u32 offset, bool write) const override { return write; }
    // The timers change only at the host's ticks and at writes.
    bool ReadStable(u32 offset) const override { return true; }

    void BindInterrupt(int id, int q, InterruptHandler callback);
    // Called before a register write changes anything: the host counts the
    // clock ticks that came before it (BlackFinCpu::AdvanceGpTimers).
    void SetBeforeWrite(std::function<void()> callback) { beforeWrite_ = std::move(callback); }

    void Tick(GPTimerClockType clockType);
    // Many ticks of a clock at once, as that many Ticks (TMRCLK's, which
    // runs at MHz), and how many can come before a timer's state changes
    // (UINT64_MAX: none that counts it runs).
    void Advance(GPTimerClockType clockType, uint64_t ticks);
    uint64_t TicksToChange(GPTimerClockType clockType) const;
    // SCLK cycles per TMRCLK cycle, 0 while nothing clocks it. On the OP-1
    // TMRCLK (the PPI clock pin, which TIMER0/1, the PPI's frame syncs,
    // count) is taken to be TIMER7's PWM output, which the firmware runs as
    // a continuous SCLK/14 clock and uses for nothing else seen: inferred
    // from the firmware, not from the board's wiring.
    u32 TmrclkSclks() const;

private:
    std::array<std::shared_ptr<GPTimerImpl>, 8> timers;
    std::function<void()> beforeWrite_;
    // TIMER_STATUS as last read, while statusValid_.
    u32 status_ = 0;
    bool statusValid_ = false;
};