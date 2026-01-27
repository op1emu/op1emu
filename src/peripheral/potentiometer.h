#pragma once

#include "cpu/twi.h"

class Potentiometer : public RegisterI2CPeripheral {
public:
    Potentiometer(u32 addr);

    void SetValue(u8 value) {
        potValue = value;
    }

protected:
    u8 potValue = 0;
};