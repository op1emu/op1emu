#include "mcp230xx.h"

#define MCP230XX_IODIR 0x00
#define MCP230XX_IPOL 0x01
#define MCP230XX_INTEN 0x02
#define MCP230XX_DEFVAL 0x03
#define MCP230XX_INTCON 0x04
#define MCP230XX_IOCON 0x05
#define MCP230XX_GPPU 0x06
#define MCP230XX_INTF 0x07
#define MCP230XX_INTCAP 0x08
#define MCP230XX_GPIO 0x09
#define MCP230XX_OLAT 0x0a

#define BANKSIZE 8

#define MCP23008_INT_PIN 8
#define MCP23017_INTA_PIN 16
#define MCP23017_INTB_PIN 17

#define REG_ADDR(reg, bank) ((u32)(registerBank ? ((bank << ((gpioCount / BANKSIZE) - 1)) | reg) : (reg << ((gpioCount / BANKSIZE) - 1) | bank)))
#define REG_ADDR2BANK(addr) (registerBank ? ((addr >> ((gpioCount / BANKSIZE) - 1)) & 0x1) : (addr & 0x1))
#define REG_ADDR2REG(addr) (registerBank ? (addr & ((1 << ((gpioCount / BANKSIZE) - 1)) - 1)) : (addr >> ((gpioCount / BANKSIZE) - 1)))

#define FIELD8(reg, f, r, w) reg.fields[#f] = { .offset = 0, .length = 8, .readCallback = r, .writeCallback = w }
#undef R
#undef W
#define R(x) [bank, this]() -> u32 { return x; }
#define W(x) [bank, this](u32 v) { x = static_cast<u8>(v); }


MCP230XX::MCP230XX(u32 address, MCP230XXModel model) : RegisterI2CPeripheral(address), model(model) {
    gpioCount = (model == MCP230XXModel::MCP23008) ? 8 : 16;
    registerCount = (model == MCP230XXModel::MCP23008) ? 11 : 22;
    registerBank = (model == MCP230XXModel::MCP23008) ? true : false;

    SwitchRegisterBank();
}

int MCP230XX::GetPinCount() const {
    // 8 + INT or 16 + INTA + INTB
    return model == MCP230XXModel::MCP23008 ? gpioCount + 1 : gpioCount + 2;
}

void MCP230XX::SwitchRegisterBank() {
    registers.clear();
    for (int bank = 0; bank < gpioCount / BANKSIZE; bank++) {
        REG32(IODIR, REG_ADDR(MCP230XX_IODIR, bank));
        FIELD8(IODIR, IODIR, R(iodirInput[bank]), W(iodirInput[bank]));

        REG32(IPOL, REG_ADDR(MCP230XX_IPOL, bank));
        FIELD8(IPOL, IPOL, R(ipol[bank]), W(ipol[bank]));

        REG32(INTEN, REG_ADDR(MCP230XX_INTEN, bank));
        auto inten_write_callback = [bank, this](u32 v) {
            u8 prev = inten[bank];
            inten[bank] = static_cast<u8>(v);
            // FIXME: figure out correct behavior here
            if (prev != inten[bank]) {
                intcap[bank] = level[bank];
            }
        };
        FIELD8(INTEN, INTEN, R(inten[bank]), inten_write_callback);

        REG32(DEFVAL, REG_ADDR(MCP230XX_DEFVAL, bank));
        FIELD8(DEFVAL, DEFVAL, R(defaultValue[bank]), W(defaultValue[bank]));

        REG32(INTCON, REG_ADDR(MCP230XX_INTCON, bank));
        FIELD8(INTCON, INTCON, R(intCompareDef[bank]), W(intCompareDef[bank]));

        REG32(IOCON, REG_ADDR(MCP230XX_IOCON, bank));
        FIELD(IOCON, INTCC, 0, 1, R(intClearOnReadIntcap ? 1 : 0), W(intClearOnReadIntcap));
        FIELD(IOCON, INTPOL, 1, 1, R(intActiveHigh ? 1 : 0), W(intActiveHigh));
        FIELD(IOCON, ODR, 2, 1, R(intOutputOpenDrain ? 1 : 0), W(intOutputOpenDrain));
        FIELD(IOCON, SEQOP, 5, 1, R(byteMode ? 1 : 0), W(byteMode));
        FIELD(IOCON, MIRROR, 6, 1, R(intMirror ? 1 : 0), W(intMirror));
        FIELD(IOCON, BANK, 7, 1, R(registerBank ? 1 : 0), W(registerBank));
        IOCON.writeCallback = [this](u32 value) {
            SwitchRegisterBank();
            ForwardInterrupt(0);
            if (model != MCP230XXModel::MCP23008) {
                ForwardInterrupt(1);
            }
        };

        REG32(GPPU, REG_ADDR(MCP230XX_GPPU, bank));
        FIELD8(GPPU, GPPU, R(pullup[bank]), W(pullup[bank]));

        REG32(INTF, REG_ADDR(MCP230XX_INTF, bank));
        FIELD8(INTF, INTF, R(intFlag[bank]), N());

        REG32(INTCAP, REG_ADDR(MCP230XX_INTCAP, bank));
        auto intcap_read_callback = [bank, this]() -> u32 {
            u8 val = intcap[bank];
            if (intClearOnReadIntcap) {
                ClearInterrupt(bank);
            }
            return val;
        };
        FIELD8(INTCAP, INTCAP, intcap_read_callback, N());

        REG32(GPIO, REG_ADDR(MCP230XX_GPIO, bank));
        auto gpio_write_callback = [bank, this](u32 v) {
            // Only affect output pins
            level[bank] &= iodirInput[bank];
            level[bank] |= static_cast<u8>(v) & ~iodirInput[bank];
            olat[bank] = level[bank] & ~iodirInput[bank];
        };
        auto gpio_read_callback = [bank, this]() -> u32 {
            for (int i = 0; i < BANKSIZE; i++) {
                u8 bit = 1 << i;
                int pin = i + bank * BANKSIZE;
                if ((iodirInput[bank] & bit) && (inputConnected[bank] & bit) == 0) {
                    // If pull-up enabled on unconnected input, set to high
                    if (pullup[bank] & bit) {
                        level[bank] |= bit;
                    } else {
                        level[bank] &= ~bit;
                    }
                }
            }
            if (!intClearOnReadIntcap) {
                ClearInterrupt(bank);
            }
            return level[bank] ^ ipol[bank];
        };
        FIELD8(GPIO, GPIO, gpio_read_callback, gpio_write_callback);

        REG32(OLAT, REG_ADDR(MCP230XX_OLAT, bank));
        FIELD8(OLAT, OLAT, R(olat[bank]), W(olat[bank]));
    }
}

bool MCP230XX::Write(const u8* buffer, u32 length) {
    int bank = writeRegister ? REG_ADDR2BANK(writeRegister->addr) : -1;
    int reg = writeRegister ? REG_ADDR2REG(writeRegister->addr) : -1;
    if (!RegisterI2CPeripheral::Write(buffer, length)) {
        return false;
    }
    // register bank may have changed, re-select register
    if (bank != -1 && reg != -1) {
        writeRegister = &registers[REG_ADDR(reg, bank)];
        readRegister = writeRegister;
    }
    return true;
}

Register* MCP230XX::Next(u32 addr) const {
    if (!registerBank && byteMode && model == MCP230XXModel::MCP23017) {
        // toggle mode
        u32 bank = addr & 0x1;
        u32 next = (bank + 1) % 2;

        addr = (addr & ~0x1) | next;
    } else if (!byteMode) {
        addr = (addr + 1) % registerCount;
    }
    auto iter = registers.find(addr);
    if (iter != registers.end()) {
        return const_cast<Register*>(&iter->second);
    }
    return nullptr;
}

bool MCP230XX::SetPinInput(int pin, GPIOPinLevel l) {
    if (pin < 0 || pin >= gpioCount) return false;

    int bank = pin / 8;
    int bit = 1 << (pin % 8);

    // Only process if pin is input
    if ((iodirInput[bank] & bit) == 0) {
        return false;
    }

    inputConnected[bank] |= bit;
    u8 prevLevel = level[bank];
    if (l == GPIOPinLevel::High) {
        level[bank] |= bit;
    } else {
        level[bank] &= ~bit;
    }
    // Check for interrupt
    if (!intActive[bank] && (inten[bank] & bit)) {
        bool trigger = false;
        if (intCompareDef[bank] & bit) {
            // Compare to DEFVAL
            if ((level[bank] & bit) != (defaultValue[bank] & bit)) {
                trigger = true;
            }
        } else {
            // Change from previous state
            if ((prevLevel & bit) != (level[bank] & bit)) {
                trigger = true;
            }
        }
        if (trigger) {
            intFlag[bank] = bit;
            intcap[bank] = level[bank];
            intActive[bank] = true;
        }
        ForwardInterrupt(bank);
    }
    return true;
}

GPIOPinLevel MCP230XX::GetPinOutput(int pin) const {
    if (pin < 0 || pin >= GetPinCount()) return GPIOPinLevel::Low;

    if (pin >= gpioCount) {
        // Interrupt output pins
        bool active = false;
        if (model == MCP230XXModel::MCP23008) {
            active = intActive[0];
        } else {
            if (!intMirror) {
                active = intActive[pin - MCP23017_INTA_PIN];
            } else {
                active = intActive[0] || intActive[1];
            }
        }
        if (intOutputOpenDrain) {
            // TODO: Open-drain output handling
            return active ? GPIOPinLevel::High : GPIOPinLevel::Low;
        } else {
            if ((active && intActiveHigh) || (!active && !intActiveHigh)) {
                return GPIOPinLevel::High;
            } else {
                return GPIOPinLevel::Low;
            }
        }
    }

    int bank = pin / 8;
    int bit = 1 << (pin % 8);

    if ((level[bank] & bit) == (ipol[bank] & bit)) {
        return GPIOPinLevel::Low;
    } else {
        return GPIOPinLevel::High;
    }
}

GPIOPinDirection MCP230XX::GetDirection(int pin) const {
    if (pin < 0 || pin >= GetPinCount()) return GPIOPinDirection::Input;

    if (pin >= gpioCount) {
        // Interrupt output pins
        return GPIOPinDirection::Output;
    }

    int bank = pin / 8;
    int bit = 1 << (pin % 8);

    if ((iodirInput[bank] & bit) == 0) {
        return GPIOPinDirection::Output;
    } else {
        return GPIOPinDirection::Input;
    }
}

void MCP230XX::ForwardInterrupt(int bank) {
    if (model != MCP230XXModel::MCP23017) {
        ForwardConnections(MCP23008_INT_PIN);
    } else {
        if (!intMirror) {
            ForwardConnections(MCP23017_INTA_PIN + bank);
        } else {
            ForwardConnections(MCP23017_INTA_PIN);
            ForwardConnections(MCP23017_INTB_PIN);
        }
    }
}

void MCP230XX::ClearInterrupt(int bank) {
    // Clear interrupt flags for this bank
    intFlag[bank] = 0;
    // Clear interrupt level output
    intActive[bank] = false;
    // Set input pins again
    for (int i = 0; i < BANKSIZE; i++) {
        int pin = i + bank * BANKSIZE;
        if (inputConnected[bank] & (1 << i)) {
            SetPinInput(pin, (level[bank] & (1 << i)) ? GPIOPinLevel::High : GPIOPinLevel::Low);
        }
    }
    ForwardInterrupt(bank);
}