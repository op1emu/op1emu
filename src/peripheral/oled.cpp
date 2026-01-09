#include "oled.h"
#include "utils/log.h"

static constexpr int CS_PIN = 0;
static constexpr int RS_PIN = 1;
static constexpr int RD_PIN = 2;
static constexpr int WR_PIN = 3;
static constexpr int PIN_COUNT = WR_PIN + 1;

OLED::OLED(std::array<GPIOConnection, OLED::DATA_PINS> bus, GPIOConnection cs, GPIOConnection rs, GPIOConnection rd, GPIOConnection wr)
    : dataBus(bus) {
    // Connect control pins
    Connect(CS_PIN, cs);
    Connect(RS_PIN, rs);
    Connect(RD_PIN, rd);
    Connect(WR_PIN, wr);

    for (u32 i = 0xf01; i <= 0xf0E; i++) {
        REG32(REG_INIT_SETTINGS, i);
    }
    for (u32 i = 0xf10; i <= 0xf17; i++) {
        REG32(REG_RAM_SETTINGS, i);
    }
    for (u32 i = 0xf20; i <= 0xf27; i++) {
        REG32(REG_POWER_SETTINGS, i);
    }
    for (u32 i = 0xf30; i <= 0xf41; i++) {
        REG32(REG_GAMMA_SETTINGS, i);
    }
}

GPIOPinDirection OLED::GetDirection(int pin) const {
    return GPIOPinDirection::Input;
}

int OLED::GetPinCount() const {
    return PIN_COUNT;
}

GPIOPinLevel OLED::GetPinOutput(int pin) const {
    return GPIOPinLevel::Low;
}

u32 OLED::ReadFromBus() const {
    u32 value = 0;
    for (int i = 0; i < DATA_PINS; i++) {
        auto& [conn, pin] = dataBus[i];
        GPIOPinLevel level = conn.GetPinOutput(pin);
        if (level == GPIOPinLevel::High) {
            value |= (1 << i);
        }
    }
    return value;
}

void OLED::WriteToBus(u32 value) const {
    for (int i = 0; i < DATA_PINS; i++) {
        auto& [conn, pin] = dataBus[i];
        GPIOPinLevel level = (value & (1 << i)) ? GPIOPinLevel::High : GPIOPinLevel::Low;
        conn.SetPinInput(pin, level);
    }
}

bool OLED::SetPinInput(int pin, GPIOPinLevel level) {
    if (pin == CS_PIN) {
        chipSelection = (level == GPIOPinLevel::Low); // Active low
        if (!chipSelection) {
            // Deselecting chip, clear state
            read = false;
            write = false;
            registerSelection = false;
            data = 0;
            selectedRegister = nullptr;
        }
        return true;
    } else if (pin == RS_PIN) {
        registerSelection = (level == GPIOPinLevel::Low); // Active low
        return true;
    } else if (pin == RD_PIN) {
        read = (level == GPIOPinLevel::Low); // Active low
        if (read && chipSelection) {
            // Perform read operation
            if (!registerSelection) {
                // Data read
                if (selectedRegister) {
                    data = selectedRegister->Read32();
                    WriteToBus(data);
                } else {
                    // No register selected
                    LogDebug("OLED: No register selected for data read");
                }
            }
        }
        return true;
    } else if (pin == WR_PIN) {
        write = (level == GPIOPinLevel::Low); // Active low
        if (write && chipSelection) {
            data = ReadFromBus();
            // Perform write operation
            if (registerSelection) {
                // Register select
                auto iter = registers.find(data);
                if (iter != registers.end()) {
                    selectedRegister = &iter->second;
                }
            } else {
                // Data write
                if (selectedRegister) {
                    selectedRegister->Write32(data);
                } else {
                    // No register selected
                    LogDebug("OLED: No register selected for data write");
                }
            }
        }
        return true;
    }
    return false;
}