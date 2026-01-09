#pragma once

#include "io.h"
#include "dma.h"
#include <memory>

class Display;
class PPI : public RegisterDevice, public DMABus {
public:
    PPI(u32 baseAddr);

    // Only support Output mode
    u32 DMARead(int x, int y, void* dest, u32 length) override { return 0; }
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;

    void AttachDisplay(std::shared_ptr<Display> display) {
        this->display = display;
    }

protected:
    // PPI control
    bool enabled = false;
    bool outputMode = false;
    u8 transferType = 0;
    u8 portConfig = 0;
    bool packingEnabled = false;
    u8 dataLength = 0;
    u16 rowCount = 0;
    u16 delay = 0;
    u16 lineCount = 0;

    std::shared_ptr<Display> display;
};