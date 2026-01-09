#pragma once

#include "io.h"
#include <array>
#include <memory>

class GPTimerImpl;

class GPTimer : public RegisterDevice {
public:
    GPTimer(u32 baseAddr);

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

private:
    std::array<std::shared_ptr<GPTimerImpl>, 8> timers;
};