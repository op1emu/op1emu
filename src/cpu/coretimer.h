#pragma once

#include "io.h"
#include <optional>

class TimeSource;

// The core timer counts TCOUNT down once every TSCALE + 1 core cycles of guest
// time while TMPWR and TMREN are set. At zero it sets TINT and raises IVG6,
// then reloads TPERIOD (TAUTORLD) or stops. The count is kept as an origin
// and the count it had there, so it advances without being stepped.
class CoreTimer : public RegisterDevice {
public:
    CoreTimer(u32 baseAddr, const TimeSource& time);

    // Brings the count up to guest time `cycles`, expiring as often as it
    // reached zero since the last call. The CPU calls it after every block;
    // register accesses call it at their own time.
    void Advance(u64 cycles);
    // The guest time at which the count next reaches zero, or none.
    std::optional<u64> NextExpiry() const;

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

    void ProcessWithInterrupt(int ivg) override;
    // TINT is set: ProcessWithInterrupt has work only then (and in IVG6).
    bool InterruptPending() const { return interrupt; }

protected:
    bool IsEnabled() const { return power && enabled; }
    u64 CyclesPerTick() const;
    u32 CountAt(u64 cycles) const;
    // Starts or stops counting when TMPWR/TMREN changed, at guest time `cycles`.
    void UpdateCounting(u64 cycles);
    // TCOUNT has a new value, at guest time `cycles`.
    void Reload(u64 cycles);
    void UpdateInterrups();

    bool power      = false;
    bool enabled    = false;
    bool autoReload = false;
    bool interrupt  = false;
    u8  tscale  = 0;
    u32 tperiod = 0;
    u32 tcount  = 0;  // the count while stopped; at origin_ while counting
    std::optional<u64> origin_;  // guest cycles at which tcount held, while counting
    const TimeSource& time_;
};
