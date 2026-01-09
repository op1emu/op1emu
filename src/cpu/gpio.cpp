#include "gpio.h"
#include <cstdio>

static constexpr int GPIO_PIN_COUNT = 16;

GPIO::GPIO(const std::string& name, u32 baseAddr) : RegisterDevice(name, baseAddr, 0x44) {
    REG32(GPIO_DATA, 0x00);
    FIELD(GPIO_DATA, VAL, 0, 16, R(data), [this](u32 v) {
        u16 oldData = data;
        data = v;
        OnDataChanged(oldData);
    });
    REG32(GPIO_CLEAR, 0x04);
    FIELD(GPIO_CLEAR, VAL, 0, 16, R(data), [this](u32 v) {
        u16 oldData = data;
        data &= ~v;
        OnDataChanged(oldData);
        // Clear edge interrupts for cleared bits
        bool changed = false;
        for (int i = 0; i < GPIO_PIN_COUNT; i++) {
            u16 bit = 1 << i;
            if (v & bit && edge & bit) {
                intState &= ~bit;
                changed = true;
            }
        }
        if (changed) {
            ForwardInterrupts();
        }
    });
    REG32(GPIO_SET, 0x08);
    FIELD(GPIO_SET, VAL, 0, 16, R(data), [this](u32 v) {
        u16 oldData = data;
        data |= v;
        OnDataChanged(oldData);
    });
    REG32(GPIO_TOGGLE, 0x0C);
    FIELD(GPIO_TOGGLE, VAL, 0, 16, R(data), [this](u32 v) {
        u16 oldData = data;
        data ^= v;
        OnDataChanged(oldData);
    });

    REG32(GPIO_MASKA, 0x10);
    FIELD(GPIO_MASKA, maskA, 0, 16, R(maskA), W(maskA));
    GPIO_MASKA.writeCallback = [this](u32 v) { ForwardInterrupt(irqA, maskA); };
    REG32(GPIO_MASKA_CLEAR, 0x14);
    FIELD(GPIO_MASKA_CLEAR, maskA, 0, 16, R(maskA), [this](u32 v) {
        maskA &= ~v;
    });
    GPIO_MASKA_CLEAR.writeCallback = GPIO_MASKA.writeCallback;
    REG32(GPIO_MASKA_SET, 0x18);
    FIELD(GPIO_MASKA_SET, maskA, 0, 16, R(maskA), [this](u32 v) {
        maskA |= v;
    });
    GPIO_MASKA_SET.writeCallback = GPIO_MASKA.writeCallback;
    REG32(GPIO_MASKA_TOGGLE, 0x1C);
    FIELD(GPIO_MASKA_TOGGLE, maskA, 0, 16, R(maskA), [this](u32 v) {
        maskA ^= v;
    });
    GPIO_MASKA_TOGGLE.writeCallback = GPIO_MASKA.writeCallback;

    REG32(GPIO_MASKB, 0x20);
    FIELD(GPIO_MASKB, maskB, 0, 16, R(maskB), [this](u32 v) {
        maskB = v;
    });
    GPIO_MASKB.writeCallback = [this](u32 v) { ForwardInterrupt(irqB, maskB); };
    REG32(GPIO_MASKB_CLEAR, 0x24);
    FIELD(GPIO_MASKB_CLEAR, maskB, 0, 16, R(maskB), [this](u32 v) {
        maskB &= ~v;
    });
    GPIO_MASKB_CLEAR.writeCallback = GPIO_MASKB.writeCallback;
    REG32(GPIO_MASKB_SET, 0x28);
    FIELD(GPIO_MASKB_SET, maskB, 0, 16, R(maskB), [this](u32 v) {
        maskB |= v;
    });
    GPIO_MASKB_SET.writeCallback = GPIO_MASKB.writeCallback;
    REG32(GPIO_MASKB_TOGGLE, 0x2C);
    FIELD(GPIO_MASKB_TOGGLE, maskB, 0, 16, R(maskB), [this](u32 v) {
        maskB ^= v;
    });
    GPIO_MASKB_TOGGLE.writeCallback = GPIO_MASKB.writeCallback;

    REG32(GPIO_DIR, 0x30);
    FIELD(GPIO_DIR, dir, 0, 16, R(dir_output), W(dir_output));
    GPIO_DIR.writeCallback = [this](u32 v) {
        // When direction changes, re-evaluate all pins for connection forwarding
        for (int pin = 0; pin < GPIO_PIN_COUNT; pin++) {
            ForwardConnections(pin);
        }
    };

    REG32(GPIO_POLAR, 0x34);
    FIELD(GPIO_POLAR, polar, 0, 16, R(polar_active_low), W(polar_active_low));
    GPIO_POLAR.writeCallback = [this](u32 v) {
        // When polarity changes, re-evaluate all pins for connection forwarding
        for (int pin = 0; pin < GPIO_PIN_COUNT; pin++) {
            ForwardConnections(pin);
        }
    };

    REG32(GPIO_EDGE, 0x38);
    FIELD(GPIO_EDGE, edge, 0, 16, R(edge), W(edge));

    REG32(GPIO_BOTH, 0x3C);
    FIELD(GPIO_BOTH, both, 0, 16, R(both), W(both));

    REG32(GPIO_INEN, 0x40);
    FIELD(GPIO_INEN, inen, 0, 16, R(inen), W(inen));
}

int GPIO::GetPinCount() const {
    return GPIO_PIN_COUNT;
}

GPIOPinDirection GPIO::GetDirection(int pin) const {
    if (pin < 0 || pin >= GPIO_PIN_COUNT) {
        return GPIOPinDirection::Input;
    }
    return (dir_output & (1 << pin)) ? GPIOPinDirection::Output : GPIOPinDirection::Input;
}

GPIOPinLevel GPIO::GetPinOutput(int pin) const {
    if (pin < 0 || pin >= GPIO_PIN_COUNT) return GPIOPinLevel::Low;
    u16 bit = 1 << pin;
    if ((data & bit) == (polar_active_low & bit)) {
        return GPIOPinLevel::Low;
    } else {
        return GPIOPinLevel::High;
    }
}

bool GPIO::SetPinInput(int pin, GPIOPinLevel level) {
    if (pin < 0 || pin >= GPIO_PIN_COUNT) return false;

    u16 bit = 1 << pin;

    // Only process if pin is input and input is enabled
    if ((dir_output & bit) || !(inen & bit)) {
        return false;
    }

    GPIOPinLevel oldLevel = GetPinOutput(pin);
    // Update data
    if (level == GPIOPinLevel::Low && (polar_active_low & bit)) {
        data |= bit;
    } else if (level == GPIOPinLevel::High && !(polar_active_low & bit)) {
        data |= bit;
    } else {
        data &= ~bit;
    }
    GPIOPinLevel newLevel = level;

    bool triggerInterrupt = false;
    // Check for interrupt generation
    if (edge & bit) {
        // Edge triggered
        if (both & bit) {
            // Both edges
            triggerInterrupt = oldLevel != newLevel;
        } else {
            // Single edge based on polarity
            bool risingEdge = oldLevel == GPIOPinLevel::Low && newLevel == GPIOPinLevel::High;
            bool fallingEdge = oldLevel == GPIOPinLevel::High && newLevel == GPIOPinLevel::Low;
            bool polarityHigh = (polar_active_low & bit) == 0;
            if (polarityHigh) {
                triggerInterrupt = risingEdge;
            } else {
                triggerInterrupt = fallingEdge;
            }
        }
        // Generate edge interrupt
        if (triggerInterrupt) {
            intState |= bit;
            ForwardInterrupts();
        }
    } else {
        // Level triggered
        bool polarityHigh = (polar_active_low & bit) == 0;
        if (newLevel == GPIOPinLevel::High) {
            intState &= ~bit;
        } else {
            intState |= bit;
        }
        ForwardInterrupts();
    }
    return true;
}

void GPIO::ForwardInterrupts() {
    ForwardInterrupt(irqA, maskA);
    ForwardInterrupt(irqB, maskB);
}

void GPIO::ForwardInterrupt(int irq, u16 mask) {
    // If any masked bits have interrupts pending, trigger
    if (intState & mask) {
        TriggerInterrupt(irq, 1);
    } else {
        TriggerInterrupt(irq, 0);
    }
}

void GPIO::OnDataChanged(u16 oldData) {
    for (int pin = 0; pin < GPIO_PIN_COUNT; pin++) {
        if (((1 << pin) & (oldData ^ data)) != 0) {
            ForwardConnections(pin);
        }
    }
}