#pragma once

#include "io.h"

class EBIU : public RegisterDevice {
public:
    EBIU(u32 baseAddr);

private:
    int type;

    // Register values
    u32 sdgctl;
    u16 sdbctl;
    u16 sdrrc;
    u16 sdstat;
    u16 sdstat_sdease;
};