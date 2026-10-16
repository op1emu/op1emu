#pragma once

#include "io.h"
#include <functional>

class SIC : public RegisterDevice {
public:
    SIC(u32 baseAddr);

    // Called when an IVG line goes from low to high (bit n: IVGn).
    void SetLineRaisedCallback(std::function<void(u16 rising)> callback) {
        lineRaised = std::move(callback);
    }

    // Called when a peripheral raises/lowers an interrupt
    void SetInterruptLevel(int pin, int level);

    // The SIC's outputs to the core, as levels (bit n: IVGn): a line is high
    // while any pin routed to it is asserted (SIC_ISR) and unmasked
    // (SIC_IMASK). Peripheral interrupts are level-sensitive: the core latches
    // a line that is high, again whenever it is high and neither latched nor
    // being serviced.
    u16 Lines() const { return lines_; }

    void SetBootMode(u8 mode) { bmode = mode; }

    // Core IVG (7..15) that system interrupt `pin` is currently routed to by
    // SIC_IARx, or -1 if `pin` is out of range.
    int GetIVG(int pin) const;

private:
    void InitRegisters();
    void UpdateLines();

    u16 rvect = 0;
    u8 bmode = 0;
    u32 imask[2] = {0};
    u32 iar[8] = {0};
    u32 isr[2] = {0};
    u32 iwr[2] = {0};
    u16 lines_ = 0;

    std::function<void(u16 rising)> lineRaised;
};