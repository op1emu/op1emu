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

    int GetPinCount() const override { return gpioCount; }

protected:
    void SwitchRegisterBank();

    MCP230XXModel model;
    int gpioCount;
    int registerCount;

    // Register state storage (2 banks for MCP23017)
    std::array<u8, 2> iodir_input = {0xFF, 0xFF};
    std::array<u8, 2> ipol = {0, 0};
    std::array<u8, 2> inten = {0, 0};
    std::array<u8, 2> defaultValue = {0, 0};
    std::array<u8, 2> pullup = {0, 0};
    std::array<u8, 2> value = {0, 0};
    std::array<u8, 2> olat = {0, 0};

    bool registerBank = false;
    bool intMirror = false;
    bool byteMode = false;
    bool intPolarity = false;

    std::function<void(int)> interruptCallback;
};