#pragma once

#include "cpu/twi.h"
#include "cpu/gpio.h"
#include <functional>
#include <array>

enum class MCP230XXModel {
    MCP23008,   // 8 GPIOs
    MCP23017,   // 16 GPIOs
};

class MCP230XX : public RegisterI2CPeripheral, public GPIOPeripheral {
public:
    MCP230XX(u32 address, MCP230XXModel model);

    bool Write(const u8* buffer, u32 length) override;
    Register* Next(u32 addr) const override;

    void SetInterruptCallback(std::function<void(int)> callback) {
        interruptCallback = callback;
    }

    GPIOPinDirection GetDirection(int pin) const override;

    bool SetPinInput(int pin, GPIOPinLevel level) override;
    GPIOPinLevel GetPinOutput(int pin) const override;

    int GetPinCount() const override;

protected:
    void SwitchRegisterBank();
    void ForwardInterrupt(int bank);
    void ClearInterrupt(int bank);

    MCP230XXModel model;
    int gpioCount;
    int registerCount;

    // Register state storage (2 banks for MCP23017)
    std::array<u8, 2> iodirInput = {0xFF, 0xFF};
    std::array<u8, 2> ipol = {0, 0};
    std::array<u8, 2> inten = {0, 0};
    std::array<u8, 2> defaultValue = {0, 0};
    std::array<u8, 2> intCompareDef = {0, 0};
    std::array<u8, 2> pullup = {0, 0};
    std::array<u8, 2> level = {0, 0};
    std::array<u8, 2> olat = {0, 0};
    std::array<u8, 2> intFlag = {0, 0};
    std::array<u8, 2> intcap = {0, 0};
    std::array<u8, 2> inputConnected = {0, 0};
    std::array<bool, 2> intActive = {false, false};

    bool registerBank = false;
    bool intMirror = false;
    bool byteMode = false;
    bool intOutputOpenDrain = false;
    bool intActiveHigh = false;
    bool intClearOnReadIntcap = false;

    std::function<void(int)> interruptCallback;
};