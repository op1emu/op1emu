#pragma once

#include "io.h"

class Jtag : public RegisterDevice {
public:
    Jtag(u32 baseAddr, u32 dspid);
    ~Jtag() override = default;

private:
    u32 dspid;
};