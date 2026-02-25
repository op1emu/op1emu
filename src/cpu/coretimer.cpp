#include "coretimer.h"

constexpr int IVG_IVTMR = 6;

CoreTimer::CoreTimer(u32 baseAddr) : RegisterDevice("CoreTimer", baseAddr, 0x10) {
    REG32(TCNTL, 0x00);
    FIELD(TCNTL, TMPWR, 0, 1, R(power), W(power));
    FIELD(TCNTL, TMREN, 1, 1, R(enabled), W(enabled));
    FIELD(TCNTL, TAUTORLD, 2, 1, R(autoReload), W(autoReload));
    FIELD(TCNTL, TINT, 3, 1, R(interrupt), W(interrupt));
    TCNTL.writeCallback = [this](u32 v) {
        if (enabled) {
            startCycles = 0; // This will be set to actual cycle count on next UpdateCycles call
        }
    };

    REG32(TPERIOD, 0x04);
    FIELD(TPERIOD, VAL, 0, 32, R(tperiod), W(tperiod));
    TPERIOD.writeCallback = [this](u32 v) {
        // Writes to TPERIOD are mirrored into TCOUNT
        tcount = v;
    };

    REG32(TSCALE, 0x08);
    FIELD(TSCALE, VAL, 0, 8, R(tscale), W(tscale));

    REG32(TCOUNT, 0x0C);
    FIELD(TCOUNT, VAL, 0, 32, R(tcount), W(tcount));
}

void CoreTimer::UpdateCycles(u64 cycles)
{
    if (!IsEnabled() || tcount == 0) {
        return;
    }

    if (startCycles == 0) {
        startCycles = cycles;
    }

    u64 elapsed = cycles - startCycles;
    u64 scale = tscale + 1; // Scale is 0-based (0 means divide by 1)
    u64 ticks = elapsed / scale;

    if (ticks >= tperiod) {
        tcount = 0;
    } else {
        tcount = tperiod - ticks;
    }

    if (tcount == 0) {
        OnExpire();
    }
}

void CoreTimer::OnExpire() {
    interrupt = true;
    if (autoReload) {
        tcount = tperiod;
    } else {
        enabled = false;
    }
    UpdateInterrups();
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
    if (ivg == IVG_IVTMR) {
        interrupt = false;
        UpdateInterrups();
    }
}