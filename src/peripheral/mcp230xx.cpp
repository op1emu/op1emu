#include "mcp230xx.h"

#define MCP230XX_IODIR 0x00
#define MCP230XX_IPOL 0x01
#define MCP230XX_INTEN 0x02
#define MCP230XX_DEFVAL 0x03
#define MCP230XX_INTCON 0x04
#define MCP230XX_IOCON 0x05
#define MCP230XX_GPPU 0x06
#define MCP230XX_GPIO 0x09
#define MCP230XX_OLAT 0x0a

#define BANKSIZE 8

#define REG_ADDR(reg, bank) ((u32)(registerBank ? ((bank << ((gpioCount / BANKSIZE) - 1)) | reg) : (reg << ((gpioCount / BANKSIZE) - 1) | bank)))

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

void MCP230XX::SwitchRegisterBank() {
    registers.clear();
    for (int bank = 0; bank < gpioCount / BANKSIZE; bank++) {
        REG32(IODIR, REG_ADDR(MCP230XX_IODIR, bank));
        FIELD8(IODIR, IODIR, R(iodir_input[bank]), W(iodir_input[bank]));

        REG32(IPOL, REG_ADDR(MCP230XX_IPOL, bank));
        FIELD8(IPOL, IPOL, R(ipol[bank]), W(ipol[bank]));

        REG32(INTEN, REG_ADDR(MCP230XX_INTEN, bank));
        FIELD8(INTEN, INTEN, R(inten[bank]), W(inten[bank]));

        REG32(DEFVAL, REG_ADDR(MCP230XX_DEFVAL, bank));
        FIELD8(DEFVAL, DEFVAL, R(defaultValue[bank]), W(defaultValue[bank]));

        REG32(IOCON, REG_ADDR(MCP230XX_IOCON, bank));
        FIELD(IOCON, INTPOL, 1, 1, R(intPolarity ? 1 : 0), W(intPolarity));
        FIELD(IOCON, SEQOP, 5, 1, R(byteMode ? 1 : 0), W(byteMode));
        FIELD(IOCON, MIRROR, 6, 1, R(intMirror ? 1 : 0), W(intMirror));
        FIELD(IOCON, BANK, 7, 1, R(registerBank ? 1 : 0), W(registerBank));
        IOCON.writeCallback = [this](u32 value) {
            SwitchRegisterBank();
        };

        REG32(GPPU, REG_ADDR(MCP230XX_GPPU, bank));
        FIELD8(GPPU, GPPU, R(pullup[bank]), W(pullup[bank]));

        REG32(GPIO, REG_ADDR(MCP230XX_GPIO, bank));
        auto gpio_write_callback = [bank, this](u32 v) {
            value[bank] = static_cast<u8>(v);
            olat[bank] = static_cast<u8>(v) & ~iodir_input[bank];
        };
        FIELD8(GPIO, GPIO, R(value[bank] ^ ipol[bank]), gpio_write_callback);

        REG32(OLAT, REG_ADDR(MCP230XX_OLAT, bank));
        FIELD8(OLAT, OLAT, R(olat[bank]), W(olat[bank]));
    }
}

bool MCP230XX::Write(const u8* buffer, u32 length) {
    if (!RegisterI2CPeripheral::Write(buffer, length)) {
        return false;
    }
    // register bank may have changed, re-select register
    selectedRegister = &registers[buffer[0]];
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

bool MCP230XX::SetPinInput(int pin, GPIOPinLevel level) {
    if (pin < 0 || pin >= gpioCount) return false;

    int bank = pin / 8;
    int bit = 1 << (pin % 8);

    // Only process if pin is input
    if ((iodir_input[bank] & bit) == 0) {
        return false;
    }

    if (level == GPIOPinLevel::High && ((ipol[bank] & bit) == 0)) {
        value[bank] |= bit;
    } else if (level == GPIOPinLevel::Low && ((ipol[bank] & bit) != 0)) {
        value[bank] |= bit;
    } else {
        value[bank] &= ~bit;
    }
    return true;
}

GPIOPinLevel MCP230XX::GetPinOutput(int pin) const {
    if (pin < 0 || pin >= gpioCount) return GPIOPinLevel::Low;

    int bank = pin / 8;
    int bit = 1 << (pin % 8);

    if ((value[bank] & bit) == (ipol[bank] & bit)) {
        return GPIOPinLevel::Low;
    } else {
        return GPIOPinLevel::High;
    }
}

GPIOPinDirection MCP230XX::GetDirection(int pin) const {
    if (pin < 0 || pin >= gpioCount) return GPIOPinDirection::Input;

    int bank = pin / 8;
    int bit = 1 << (pin % 8);

    if ((iodir_input[bank] & bit) == 0) {
        return GPIOPinDirection::Output;
    } else {
        return GPIOPinDirection::Input;
    }
}