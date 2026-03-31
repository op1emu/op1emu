#pragma once

#include "io.h"
#include <array>
#include <memory>

class GPTimerImpl;

enum GPTimerClockType {
    GPTimerClockTypeSCLK = 0,
    GPTimerClockTypeTACLK = 1,
    GPTimerClockTypeTMRCLK = 2,
};

class GPTimer : public RegisterDevice {
public:
    GPTimer(u32 baseAddr);
    ~GPTimer();

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

    void BindInterrupt(int id, int q, InterruptHandler callback);

    void Tick(GPTimerClockType clockType);

private:
    std::array<std::shared_ptr<GPTimerImpl>, 8> timers;
};