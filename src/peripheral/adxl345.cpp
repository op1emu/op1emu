#include "adxl345.h"

// ADXL345 Register addresses
static constexpr u8 ADXL345_REG_DEVID            = 0x00;
static constexpr u8 ADXL345_REG_THRESH_TAP       = 0x1D;
static constexpr u8 ADXL345_REG_OFSX             = 0x1E;
static constexpr u8 ADXL345_REG_OFSY             = 0x1F;
static constexpr u8 ADXL345_REG_OFSZ             = 0x20;
static constexpr u8 ADXL345_REG_DUR              = 0x21;
static constexpr u8 ADXL345_REG_LATENT           = 0x22;
static constexpr u8 ADXL345_REG_WINDOW           = 0x23;
static constexpr u8 ADXL345_REG_THRESH_ACT       = 0x24;
static constexpr u8 ADXL345_REG_THRESH_INACT     = 0x25;
static constexpr u8 ADXL345_REG_TIME_INACT       = 0x26;
static constexpr u8 ADXL345_REG_ACT_INACT_CTL    = 0x27;
static constexpr u8 ADXL345_REG_THRESH_FF        = 0x28;
static constexpr u8 ADXL345_REG_TIME_FF          = 0x29;
static constexpr u8 ADXL345_REG_TAP_AXES         = 0x2A;
static constexpr u8 ADXL345_REG_ACT_TAP_STATUS   = 0x2B;
static constexpr u8 ADXL345_REG_BW_RATE          = 0x2C;
static constexpr u8 ADXL345_REG_POWER_CTL        = 0x2D;
static constexpr u8 ADXL345_REG_INT_ENABLE       = 0x2E;
static constexpr u8 ADXL345_REG_INT_MAP          = 0x2F;
static constexpr u8 ADXL345_REG_INT_SOURCE       = 0x30;
static constexpr u8 ADXL345_REG_DATA_FORMAT      = 0x31;
static constexpr u8 ADXL345_REG_DATAX0           = 0x32;
static constexpr u8 ADXL345_REG_DATAX1           = 0x33;
static constexpr u8 ADXL345_REG_DATAY0           = 0x34;
static constexpr u8 ADXL345_REG_DATAY1           = 0x35;
static constexpr u8 ADXL345_REG_DATAZ0           = 0x36;
static constexpr u8 ADXL345_REG_DATAZ1           = 0x37;
static constexpr u8 ADXL345_REG_FIFO_CTL         = 0x38;
static constexpr u8 ADXL345_REG_FIFO_STATUS      = 0x39;

// Device ID value
static constexpr u8 ADXL345_DEVID = 0xE5;

ADXL345::ADXL345(u32 addr) : RegisterI2CPeripheral(addr) {
    // Device ID register (read-only)
    REG32(DEVID, ADXL345_REG_DEVID);
    FIELD(DEVID, VAL, 0, 8, R(ADXL345_DEVID), N());

    // Tap threshold register
    REG32(THRESH_TAP, ADXL345_REG_THRESH_TAP);
    FIELD(THRESH_TAP, VAL, 0, 8, R(threshTap), W(threshTap));

    // Offset registers
    REG32(OFSX, ADXL345_REG_OFSX);
    FIELD(OFSX, VAL, 0, 8, R(ofsX), W(ofsX));

    REG32(OFSY, ADXL345_REG_OFSY);
    FIELD(OFSY, VAL, 0, 8, R(ofsY), W(ofsY));

    REG32(OFSZ, ADXL345_REG_OFSZ);
    FIELD(OFSZ, VAL, 0, 8, R(ofsZ), W(ofsZ));

    // Duration register
    REG32(DUR, ADXL345_REG_DUR);
    FIELD(DUR, VAL, 0, 8, R(dur), W(dur));

    // Latency register
    REG32(LATENT, ADXL345_REG_LATENT);
    FIELD(LATENT, VAL, 0, 8, R(latent), W(latent));

    // Window register
    REG32(WINDOW, ADXL345_REG_WINDOW);
    FIELD(WINDOW, VAL, 0, 8, R(window), W(window));

    // Activity threshold register
    REG32(THRESH_ACT, ADXL345_REG_THRESH_ACT);
    FIELD(THRESH_ACT, VAL, 0, 8, R(threshAct), W(threshAct));

    // Inactivity threshold register
    REG32(THRESH_INACT, ADXL345_REG_THRESH_INACT);
    FIELD(THRESH_INACT, VAL, 0, 8, R(threshInact), W(threshInact));

    // Inactivity time register
    REG32(TIME_INACT, ADXL345_REG_TIME_INACT);
    FIELD(TIME_INACT, VAL, 0, 8, R(timeInact), W(timeInact));

    // Activity/Inactivity control register
    REG32(ACT_INACT_CTL, ADXL345_REG_ACT_INACT_CTL);
    FIELD(ACT_INACT_CTL, VAL, 0, 8, R(actInactCtl), W(actInactCtl));

    // Free fall threshold register
    REG32(THRESH_FF, ADXL345_REG_THRESH_FF);
    FIELD(THRESH_FF, VAL, 0, 8, R(threshFF), W(threshFF));

    // Free fall time register
    REG32(TIME_FF, ADXL345_REG_TIME_FF);
    FIELD(TIME_FF, VAL, 0, 8, R(timeFF), W(timeFF));

    // Tap axes register
    REG32(TAP_AXES, ADXL345_REG_TAP_AXES);
    FIELD(TAP_AXES, VAL, 0, 8, R(tapAxes), W(tapAxes));

    // Activity/Tap status register (read-only)
    REG32(ACT_TAP_STATUS, ADXL345_REG_ACT_TAP_STATUS);
    FIELD(ACT_TAP_STATUS, VAL, 0, 8, R(actTapStatus), N());

    // Bandwidth rate register
    REG32(BW_RATE, ADXL345_REG_BW_RATE);
    FIELD(BW_RATE, VAL, 0, 8, R(bwRate), W(bwRate));

    // Power control register
    REG32(POWER_CTL, ADXL345_REG_POWER_CTL);
    FIELD(POWER_CTL, VAL, 0, 8, R(powerCtl), W(powerCtl));

    // Interrupt enable register
    REG32(INT_ENABLE, ADXL345_REG_INT_ENABLE);
    FIELD(INT_ENABLE, DATA_READY, 7, 1, R(dataReadyIntEnabled ? 1 : 0), [this](u32 v) {
        dataReadyIntEnabled = (v != 0);
        ForwardInterrupt();
    });

    // Interrupt map register
    REG32(INT_MAP, ADXL345_REG_INT_MAP);
    FIELD(INT_MAP, VAL, 0, 8, R(intMap), [this](u32 v) {
        intMap = v;
        ForwardInterrupt();
    });

    // Interrupt source register (read-only)
    REG32(INT_SOURCE, ADXL345_REG_INT_SOURCE);
    FIELD(INT_SOURCE, DATA_READY, 7, 1, R(dataReady), N());

    // Data format register
    REG32(DATA_FORMAT, ADXL345_REG_DATA_FORMAT);
    FIELD(DATA_FORMAT, INT_INVERT, 5, 1, R(intActiveLow), [this](u32 v) {
        intActiveLow = (v != 0);
        ForwardInterrupt();
    });

    // Data registers (read-only)
    REG32(DATAX0, ADXL345_REG_DATAX0);
    FIELD(DATAX0, VAL, 0, 8, [this]() {
        dataReady = false;
        ForwardInterrupt();
        return accelX & 0xFF;
    }, N());

    REG32(DATAX1, ADXL345_REG_DATAX1);
    FIELD(DATAX1, VAL, 0, 8, [this]() {
        dataReady = false;
        ForwardInterrupt();
        return (accelX >> 8) & 0xFF;
    }, N());

    REG32(DATAY0, ADXL345_REG_DATAY0);
    FIELD(DATAY0, VAL, 0, 8, [this]() {
        dataReady = false;
        ForwardInterrupt();
        return accelY & 0xFF;
    }, N());

    REG32(DATAY1, ADXL345_REG_DATAY1);
    FIELD(DATAY1, VAL, 0, 8, [this]() {
        dataReady = false;
        ForwardInterrupt();
        return (accelY >> 8) & 0xFF;
    }, N());

    REG32(DATAZ0, ADXL345_REG_DATAZ0);
    FIELD(DATAZ0, VAL, 0, 8, [this]() {
        dataReady = false;
        ForwardInterrupt();
        return accelZ & 0xFF;
    }, N());
    REG32(DATAZ1, ADXL345_REG_DATAZ1);
    FIELD(DATAZ1, VAL, 0, 8, [this]() {
        dataReady = false;
        ForwardInterrupt();
        return (accelZ >> 8) & 0xFF;
    }, N());

    // FIFO control register
    REG32(FIFO_CTL, ADXL345_REG_FIFO_CTL);
    FIELD(FIFO_CTL, VAL, 0, 8, R(fifoCtl), W(fifoCtl));

    // FIFO status register (read-only)
    REG32(FIFO_STATUS, ADXL345_REG_FIFO_STATUS);
    FIELD(FIFO_STATUS, VAL, 0, 8, R(fifoStatus), N());
}

void ADXL345::SetAcceleration(int16_t x, int16_t y, int16_t z) {
    accelX = x;
    accelY = y;
    accelZ = z;
    dataReady = true;
    ForwardInterrupt();
}

void ADXL345::ForwardInterrupt() {
    if (intMap & (1 << 7)) {
        ForwardConnections(1);
    } else {
        ForwardConnections(0);
    }
}

GPIOPinLevel ADXL345::GetPinOutput(int pin) const {
    u32 intStatus = const_cast<Register&>(registers.at(ADXL345_REG_INT_SOURCE)).Read32();

    GPIOPinLevel level;
    if (dataReady && dataReadyIntEnabled) {
        level = intActiveLow ? GPIOPinLevel::Low : GPIOPinLevel::High;
    } else {
        level = intActiveLow ? GPIOPinLevel::High : GPIOPinLevel::Low;
    }

    if (intMap & intStatus) {
        if (pin == 1) {
            return level;
        }
    } else {
        if (pin == 0) {
            return level;
        }
    }
    return intActiveLow ? GPIOPinLevel::High : GPIOPinLevel::Low;
}