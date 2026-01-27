#include "potentiometer.h"

Potentiometer::Potentiometer(u32 addr) : RegisterI2CPeripheral(addr) {
    REG32(HIGH_VALUE, 0x00);
    FIELD(HIGH_VALUE, VAL, 0, 8, R((potValue & 0xF0) >> 4), N());
    REG32(LOW_VALUE, 0x01);
    FIELD(LOW_VALUE, VAL, 0, 8, R((potValue & 0x0F) << 4), N());
}