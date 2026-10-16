#pragma once

#include "io.h"
#include <functional>

class SIC : public RegisterDevice {
public:
    SIC(u32 baseAddr);

    // Set callback for forwarding interrupts to CEC
    void SetInterruptForwardCallback(std::function<void(int ivg, int level)> callback) {
        forwardInterrupt = callback;
    }

    // Called when a peripheral raises/lowers an interrupt
    void SetInterruptLevel(int pin, int level);

    void SetBootMode(u8 mode) { bmode = mode; }

    // Core IVG (7..15) that system interrupt `pin` is currently routed to by
    // SIC_IARx, or -1 if `pin` is out of range.
    int GetIVG(int pin) const;

    // Counts register writes: GetIVG() can only change when it does.
    const uint64_t* Version() const { return &version_; }
    void Write32(u32 offset, u32 value) override {
        ++version_;
        RegisterDevice::Write32(offset, value);
    }

private:
    void InitRegisters();
    void ForwardInterrupts();

    u16 rvect = 0;
    u8 bmode = 0;
    u32 imask[2] = {0};
    u32 iar[8] = {0};
    u32 isr[2] = {0};
    u32 iwr[2] = {0};
    uint64_t version_ = 0;

    std::function<void(int ivg, int level)> forwardInterrupt;
};