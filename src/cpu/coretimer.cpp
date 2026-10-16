#include "coretimer.h"
#include "time_source.h"

constexpr int IVG_IVTMR = 6;

CoreTimer::CoreTimer(u32 baseAddr, const TimeSource& time)
    : RegisterDevice("CoreTimer", baseAddr, 0x10), time_(time) {
    REG32(TCNTL, 0x00);
    FIELD(TCNTL, TMPWR, 0, 1, R(power), W(power));
    FIELD(TCNTL, TMREN, 1, 1, R(enabled), W(enabled));
    FIELD(TCNTL, TAUTORLD, 2, 1, R(autoReload), W(autoReload));
    FIELD(TCNTL, TINT, 3, 1, R(interrupt), W(interrupt));
    TCNTL.writeCallback = [this](u32 v) {
        UpdateCounting(time_.Cycles());
        UpdateInterrups();
    };

    REG32(TPERIOD, 0x04);
    FIELD(TPERIOD, VAL, 0, 32, R(tperiod), W(tperiod));
    TPERIOD.writeCallback = [this](u32 v) {
        // Writes to TPERIOD are mirrored into TCOUNT
        tcount = v;
        Reload(time_.Cycles());
    };

    REG32(TSCALE, 0x08);
    FIELD(TSCALE, VAL, 0, 8, R(tscale), W(tscale));

    REG32(TCOUNT, 0x0C);
    FIELD(TCOUNT, VAL, 0, 32, R(origin_ ? CountAt(time_.Cycles()) : tcount), W(tcount));
    TCOUNT.writeCallback = [this](u32 v) { Reload(time_.Cycles()); };
}

u64 CoreTimer::CyclesPerTick() const {
    // FIXME: wall time runs the emulator slower than real time, so it divides
    // the rate by TimeSource::kWallSlowdown; deterministic guest time does not.
    return u64(tscale + 1) * time_.Slowdown();
}

u32 CoreTimer::CountAt(u64 cycles) const {
    const u64 ticks = (cycles - *origin_) / CyclesPerTick();
    return ticks >= tcount ? 0 : u32(tcount - ticks);
}

std::optional<u64> CoreTimer::NextExpiry() const {
    if (!origin_) return std::nullopt;
    return *origin_ + u64(tcount) * CyclesPerTick();
}

void CoreTimer::Reload(u64 cycles) {
    // A running count restarts from the new value; an enabled timer that had
    // nothing to count starts now.
    if (origin_) origin_ = cycles;
    else UpdateCounting(cycles);
}

void CoreTimer::UpdateCounting(u64 cycles) {
    if (IsEnabled() && !origin_ && tcount != 0) {
        origin_ = cycles;
    } else if (!IsEnabled() && origin_) {
        tcount = CountAt(cycles);
        origin_.reset();
    }
}

void CoreTimer::Advance(u64 cycles) {
    if (!origin_) return;
    const u64 expiry = *NextExpiry();
    if (cycles < expiry) return;
    interrupt = true;
    UpdateInterrups();
    if (autoReload && tperiod != 0) {
        // Every period that ended by `cycles` expired; the interrupt counts once.
        const u64 period = u64(tperiod) * CyclesPerTick();
        tcount = tperiod;
        origin_ = expiry + (cycles - expiry) / period * period;
    } else {
        // The level stays published until TINT is cleared.
        tcount = 0;
        origin_.reset();
        enabled = false;
    }
}

u32 CoreTimer::Read32(u32 offset) {
    Advance(time_.Cycles());
    return RegisterDevice::Read32(offset);
}

void CoreTimer::Write32(u32 offset, u32 value) {
    const u64 now = time_.Cycles();
    Advance(now);
    // TCOUNT and TPERIOD writes restart the count from now: settle the count
    // so far into tcount first.
    if (origin_ && (offset == 0x04 || offset == 0x0C)) tcount = CountAt(now);
    RegisterDevice::Write32(offset, value);
}

void CoreTimer::UpdateInterrups() {
    if (IsEnabled() && interrupt) {
        TriggerInterrupt(1);
    } else {
        TriggerInterrupt(0);
    }
}

void CoreTimer::ProcessWithInterrupt(int ivg) {
    // Clear interrupt automatically as soon as the interrupt is serviced
    if (ivg == IVG_IVTMR && interrupt) {
        interrupt = false;
        UpdateInterrups();
    }
}
