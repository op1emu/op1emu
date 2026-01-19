#include "sic.h"

SIC::SIC(u32 baseAddr) : RegisterDevice("SIC", baseAddr, 0x100) {
    // Initialize IAR registers
    iar[0] = 0x00000000;
    iar[1] = 0x11000000;
    iar[2] = 0x33332222;
    iar[3] = 0x44444433;
    iar[4] = 0x55555555;
    iar[5] = 0x06666655;
    iar[6] = 0x33333000;
    iar[7] = 0x00000000;

    iwr[0] = 0xFFFFFFFF;
    iwr[1] = 0xFFFFFFFF;

    REG32(SWRST, 0x00);
    FIELD(SWRST, SWRST, 0, 16, R(0), N());

    REG32(SYSCR, 0x04);
    FIELD(SYSCR, SYSCR, 0, 16, R(0), N());

    REG32(RVECT, 0x08);
    FIELD(RVECT, RVECT, 0, 16, R(rvect), W(rvect));

    for (u32 i = 0; i < 2; i++) {
        REG32(IMASK, 0x0C + i * 0x40);
        auto imask_r = [this, i]() -> u32 {
            return imask[i];
        };
        auto imask_w = [this, i](u32 v) {
            imask[i] = v;
        };
        FIELD(IMASK, IMASK, 0, 32, imask_r, imask_w);
        IMASK.writeCallback = [this](u32 v) { ForwardInterrupts(); };

        // IAR0-3 - Interrupt Assignment Registers
        for (u32 j = 0; j < 4; j++) {
            int index = i * 4 + j;
            REG32(IAR, 0x10 + i * 0x40 + j * 4);
            auto r = [this, index]() -> u32 {
                return iar[index];
            };
            auto w = [this, index](u32 v) {
                iar[index] = v;
            };
            FIELD(IAR, IAR, 0, 32, r, w);
        }

        REG32(ISR, 0x20 + i * 0x40);
        auto isr_r = [this, i]() -> u32 {
            return isr[i];
        };
        FIELD(ISR, ISR, 0, 32, isr_r, N());

        REG32(IWR, 0x24 + i * 0x40);
        auto iwr_r = [this, i]() -> u32 {
            return iwr[i];
        };
        auto iwr_w = [this, i](u32 v) {
            iwr[i] = v;
        };
        FIELD(IWR, IWR, 0, 32, iwr_r, iwr_w);
    }
}

void SIC::SetInterruptLevel(int pin, int level) {
    if (pin < 0 || pin >= 64) return;

    int bank = pin / 32;
    u32 bit = 1 << (pin % 32);
    if (level) {
        isr[bank] |= bit;
    } else {
        isr[bank] &= ~bit;
    }

    ForwardInterrupts();
}

void SIC::ForwardInterrupts() {
    for (int i = 0; i < 2; i++) {
        // Process pending and unmasked interrupts
        u32 ipend = isr[i] & imask[i];

        if (!ipend) continue;

        for (int pin = 0; pin < 32; pin++) {
            u32 bit = 1 << pin;

            // This bit isn't pending, check next one
            if (!(ipend & bit)) continue;

            // The IAR registers map the System input to the Core output.
            // Every 4 bits in the IAR are used to map to IVG{7..15}.
            int iar_idx = i * 4 + pin / 8;
            int iar_off = (pin % 8) * 4;
            int iar_val = (iar[iar_idx] >> iar_off) & 0xF;

            forwardInterrupt(7 + iar_val, 1);
        }
    }
}