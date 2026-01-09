#include "ebiu.h"

EBIU::EBIU(u32 baseAddr) : RegisterDevice("EBIU", baseAddr, 0x20) {
    // Initialize register values (default power-on state)
    sdgctl = 0xE0088849;
    sdbctl = 0x0000;
    sdrrc = 0x081A;
    sdstat = 0x0001; // SDRAM is present

    REG32(EBIU_SDGCTL, 0x10);
    FIELD(EBIU_SDGCTL, sdgctl, 0, 32, R(sdgctl), W(sdgctl));

    REG32(EBIU_SDBCTL, 0x14);
    FIELD(EBIU_SDBCTL, sdbctl, 0, 16, R(sdbctl), W(sdbctl));

    REG32(EBIU_SDRRC, 0x18);
    FIELD(EBIU_SDRRC, sdrrc, 0, 12, R(sdrrc), W(sdrrc));

    REG32(EBIU_SDSTAT, 0x1C);
    FIELD(EBIU_SDSTAT, sdstat, 0, 4, R(sdstat), W(sdstat));
    FIELD(EBIU_SDSTAT, sdease, 4, 1, R(sdstat_sdease), W1C(sdstat_sdease));
}