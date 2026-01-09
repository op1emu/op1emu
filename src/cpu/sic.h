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

private:
    void InitRegisters();
    void ForwardInterrupts();

    u16 rvect = 0;
    u32 imask[2] = {0};
    u32 iar[8] = {0};
    u32 isr[2] = {0};
    u32 iwr[2] = {0};

    std::function<void(int ivg, int level)> forwardInterrupt;
};