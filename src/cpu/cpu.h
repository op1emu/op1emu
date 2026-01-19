#pragma once

#include "emu.h"
#include <memory>
#include <vector>
#include <chrono>

class BlackFinCpu;

struct BlackFinCpuWrapper {
    BlackFinCpu* host;
    void* cpu;  // SIM_CPU*

    BlackFinCpuWrapper(BlackFinCpu* host);
    ~BlackFinCpuWrapper();
};

enum RegIndex {
    FP,
    SP,
    RETS,
    R0,
    R1,
    R2,
    P1,
};

class SIC;
class GPIO;
class PPI;
class Display;
class OLED;
class NFC;
class NandFlash;
class Keyboard;
class MCP230XX;

class BlackFinCpu : public CpuInterface {
public:
    BlackFinCpu();
    ~BlackFinCpu() override;

    void HaltExecution(HaltReason reason) override;
    void SaveContext() override;
    void RestoreContext() override;
    HaltReason Run() override;

    void SetRegister(int index, u32 value) override;
    u32 GetRegister(int index) override;

    void SetPC(u32 value) override;
    u32 PC() override;

    Emulator& GetEmulator() { return emulator; }

    void QueueEvent(const std::function<void()>& event, std::chrono::nanoseconds delay = std::chrono::nanoseconds(1));

    void AttachDisplay(const std::shared_ptr<Display>& display);
    void AttachKeyboard(const std::shared_ptr<Keyboard>& keyboard);

    static BlackFinCpu& FromCPU(void* cpu);

protected:
    void ProcessInterrupt(int pin, int level);
    void ProcessEvents();

    std::shared_ptr<SIC> sic;
    std::shared_ptr<GPIO> portG;
    std::shared_ptr<PPI> ppi;
    std::shared_ptr<OLED> oled;
    std::vector<std::shared_ptr<MCP230XX>> gpioExpanders;
    std::vector<std::shared_ptr<Device>> devices;
    std::vector<std::tuple<std::chrono::nanoseconds, std::function<void()>>> eventQueue;
    std::recursive_mutex eventQueueMutex;
    std::chrono::nanoseconds elapsedTime{0};
    BlackFinCpuWrapper wrapper;
    Emulator emulator;
    uint32_t pc;
};