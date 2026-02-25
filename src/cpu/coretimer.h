#pragma once

#include "io.h"

class CoreTimer : public RegisterDevice {
public:
    CoreTimer(u32 baseAddr);

    // Called each instruction step with elapsed core clock cycles
    void UpdateCycles(u64 cycles);

    void ProcessWithInterrupt(int ivg) override;

protected:
    bool IsEnabled() const { return power && enabled; }
    void OnExpire();
    void UpdateInterrups();

    bool power      = false;
    bool enabled    = false;
    bool autoReload = false;
    bool interrupt  = false;
    u8  tscale  = 0;
    u32 tperiod = 0;
    u32 tcount  = 0;
    u64 startCycles = 0;
};