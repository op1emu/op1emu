#pragma once

#include "cpu/twi.h"
#include "cpu/gpio.h"

class ADXL345 : public RegisterI2CPeripheral, public GPIOPeripheral {
public:
    ADXL345(u32 addr);

    // Set simulated accelerometer values (in raw 10-bit format)
    void SetAcceleration(int16_t x, int16_t y, int16_t z);

    int GetPinCount() const override { return 2; } // Interrupt pins
    GPIOPinDirection GetDirection(int pin) const override { return GPIOPinDirection::Output; }
    GPIOPinLevel GetPinOutput(int pin) const override;
    bool SetPinInput(int pin, GPIOPinLevel level) override { return false; }

private:
    void ForwardInterrupt();

    // Simulated accelerometer data
    int16_t accelX = 0;
    int16_t accelY = 0;
    int16_t accelZ = 256; // Default: 1g on Z axis

    // Register values
    u8 threshTap = 0;
    u8 ofsX = 0;
    u8 ofsY = 0;
    u8 ofsZ = 0;
    u8 dur = 0;
    u8 latent = 0;
    u8 window = 0;
    u8 threshAct = 0;
    u8 threshInact = 0;
    u8 timeInact = 0;
    u8 actInactCtl = 0;
    u8 threshFF = 0;
    u8 timeFF = 0;
    u8 tapAxes = 0;
    u8 actTapStatus = 0;
    u8 bwRate = 0x0A; // Default 100Hz
    u8 powerCtl = 0;
    u8 intMap = 0;
    u8 dataFormat = 0;
    u8 fifoCtl = 0;
    u8 fifoStatus = 0;

    bool intActiveLow = false;
    bool dataReady = false;
    bool dataReadyIntEnabled = false;
};