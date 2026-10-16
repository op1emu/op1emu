#pragma once

#include "io.h"
#include <chrono>

class TimeSource;

class RTC : public RegisterDevice {
public:
    using TimePoint = std::chrono::system_clock::time_point;
    RTC(u32 baseAddr, const TimeSource& time);

    void Tick();  // Called periodically to update RTC state
    void ProcessWithInterrupt(int ivg) override;

    // Tick() has work: a register was written since the last Tick(), or the
    // calendar has reached another second. Otherwise Tick() would only
    // publish the level it last published.
    bool NeedsTick() const;
    // Deterministic mode: when NeedsTick() turns true without a write, the
    // calendar's next change (a write makes it true at once).
    TimePoint NextChange() const { return nextChange_; }

    void Write32(u32 offset, u32 value) override {
        written_ = true;
        RegisterDevice::Write32(offset, value);
    }

private:
    void UpdateInterrupts();
    u32 GetCurrentStat();
    u32 StatAt(TimePoint now) const;
    // The first time at or after `now` at which StatAt() changes.
    TimePoint NextStatChange(TimePoint now) const;

    // Between Tick()s: a register write, and what the last Tick() saw.
    bool written_ = true;
    TimePoint lastTick_;
    TimePoint nextChange_;

    bool stopwatchIntEnabled = false;
    bool alarmIntEnabled = false;
    bool secondsIntEnabled = false;
    bool minutesIntEnabled = false;
    bool hoursIntEnabled = false;
    bool hours24IntEnabled = false;
    bool dayAlarmIntEnabled = false;
    bool writeCompleteIntEnabled = false;

    bool stopwatchEvent = false;
    bool alarmEvent = false;
    bool secondsEvent = false;
    bool minutesEvent = false;
    bool hoursEvent = false;
    bool hours24Event = false;
    bool dayAlarmEvent = false;
    bool writePending = false;
    bool writeComplete = false;

    bool prescalerEnabled = false;

    u16 stopwatchCount = 0;
    u32 alarm = 0;

    u32 statShadow = 0;
    u32 lastStat = 0;
    std::chrono::system_clock::time_point baseTime;
    const TimeSource& time_;
};