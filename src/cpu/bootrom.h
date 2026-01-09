#pragma once

#include "io.h"

class BootROM : public MemoryDevice {
public:
    BootROM(u32 baseAddr);

    void BindFastMem(const std::shared_ptr<FastMem>& mem) override;
};