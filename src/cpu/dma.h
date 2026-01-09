#pragma once

#include "io.h"
#include "emu.h"
#include <vector>
#include <map>
#include <memory>

class Bus;

class DMABus {
public:
    virtual ~DMABus() {}

    virtual u32 DMARead(int x, int y, void* dest, u32 length) = 0;
    virtual u32 DMAWrite(int x, int y, const void* source, u32 length) = 0;
};

enum DMAPeripheralType {
    DMAPeripheralPPI = 0x0,
    DMAPeripheralHOSTDP = 0x1,
    DMAPeripheralNFC = 0x2,
    DMAPeripheralSPORT0Rx = 0x3,
    DMAPeripheralSPORT0Tx = 0x4,
    DMAPeripheralSPORT1Rx = 0x5,
    DMAPeripheralSPORT1Tx = 0x6,
    DMAPeripheralSPI = 0x7,
    DMAPeripheralUART0Rx = 0x8,
    DMAPeripheralUART0Tx = 0x9,
    DMAPeripheralUART1Rx = 0xA,
    DMAPeripheralUART1Tx = 0xB,
};

class DMAChannel;
class DMA : public RegisterDevice {
public:
    DMA(u32 baseAddr, Emulator& emu);

    void AttachDMABus(DMAPeripheralType type, const std::shared_ptr<DMABus>& bus) {
        dmaBuses[type] = bus;
    }

    void BindInterrupt(int channel, int q, InterruptHandler callback);

    std::shared_ptr<DMABus> GetDMABus(DMAPeripheralType type) { return dmaBuses[type]; }
    Emulator& GetEmulator() { return emulator; }

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

    void ProcessWithInterrupt(int ivg) override;

protected:
    Emulator& emulator;
    std::array<std::shared_ptr<DMAChannel>, 16> channels;
    std::map<DMAPeripheralType, std::shared_ptr<DMABus>> dmaBuses;
};