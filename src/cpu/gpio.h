#pragma once

#include "io.h"

enum class GPIOPinDirection {
    Input,
    Output,
};

enum class GPIOPinLevel {
    Low = 0,
    High = 1,
};

class GPIOPeripheral {
public:
    using GPIOConnection = std::tuple<GPIOPeripheral&, int>;
    virtual ~GPIOPeripheral() {
        for (auto& [pin, conns] : connections) {
            for (const auto& [other, otherPins] : conns) {
                for (int otherPin : otherPins) {
                    other->connections[otherPin].erase(this);
                }
            }
        }
    }

    virtual GPIOPinDirection GetDirection(int pin) const = 0;

    virtual bool SetPinInput(int pin, GPIOPinLevel level) = 0;
    virtual GPIOPinLevel GetPinOutput(int pin) const = 0;

    virtual int GetPinCount() const = 0;

    virtual void Connect(int pin, GPIOConnection otherConn) {
        auto& [other, otherPin] = otherConn;
        connections[pin][&other].push_back(otherPin);
        other.connections[otherPin][this].push_back(pin);
    }

    virtual void ForwardConnections(int pin) {
        if (GetDirection(pin) == GPIOPinDirection::Output) {
            GPIOPinLevel level = GetPinOutput(pin);
            auto iter = connections.find(pin);
            if (iter != connections.end()) {
                for (const auto& [other, otherPins] : iter->second) {
                    for (int otherPin : otherPins) {
                        other->SetPinInput(otherPin, level);
                    }
                }
            }
        }
    }

protected:
    std::map<int, std::map<GPIOPeripheral*, std::vector<int>>> connections;
};

class GPIOOrGate : public GPIOPeripheral {
public:
    GPIOOrGate(bool low) : activeLow(low) { }

    int GetPinCount() const override { return 3; } // 2 inputs, 1 output

    GPIOPinDirection GetDirection(int pin) const override {
        if (pin == 2) {
            return GPIOPinDirection::Output;
        } else {
            return GPIOPinDirection::Input;
        }
    }

    GPIOPinLevel GetPinOutput(int pin) const override {
        GPIOPinLevel output = GPIOPinLevel::Low;
        if (pin == 2) {
            // Output pin: OR of all input pins
            for (int i = 0; i < 2; i++) {
                if (inputs[i] == GPIOPinLevel::High) {
                    output = GPIOPinLevel::High;
                    break;
                }
            }
        }
        if (output == GPIOPinLevel::High) {
            return activeLow ? GPIOPinLevel::Low : GPIOPinLevel::High;
        } else {
            return activeLow ? GPIOPinLevel::High : GPIOPinLevel::Low;
        }
    }

    bool SetPinInput(int pin, GPIOPinLevel level) override {
        if (pin < 0 || pin >= 2) return false;
        if (inputs[pin] != level) {
            inputs[pin] = level;
            ForwardConnections(2); // Update output
        }
        return true;
    }

protected:
    GPIOPinLevel inputs[2];
    bool activeLow = false;
};

class GPIO : public RegisterDevice, public GPIOPeripheral {
public:
    GPIO(const std::string& name, u32 baseAddr);

    void BindInterruptA(int q, InterruptHandler callback) {
        irqA = q;
        BindInterrupt(q, callback);
    }

    void BindInterruptB(int q, InterruptHandler callback) {
        irqB = q;
        BindInterrupt(q, callback);
    }

    GPIOPinDirection GetDirection(int pin) const override;
    bool SetPinInput(int pin, GPIOPinLevel level) override;
    GPIOPinLevel GetPinOutput(int pin) const override;
    int GetPinCount() const override;

private:
    void ForwardInterrupts();
    void ForwardInterrupt(int irq, u16 mask);
    void OnDataChanged(u16 oldData);

    u16 data = 0;       // Current pin data
    u16 maskA = 0;      // Interrupt mask A
    u16 maskB = 0;      // Interrupt mask B
    u16 dir_output = 0; // Direction (1=output, 0=input)
    u16 polar_active_low = 0; // Polarity (1=active low, 0=active high)
    u16 edge = 0;       // Edge sensitivity (1=edge, 0=level)
    u16 both = 0;       // Both edges (when edge=1)
    u16 inen = 0;       // Input enable

    u16 intState = 0;   // Current interrupt state

    int irqA = 0;
    int irqB = 0;
};