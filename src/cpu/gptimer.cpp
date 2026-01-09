#include "gptimer.h"

class GPTimerImpl : public RegisterDevice {
public:
    GPTimerImpl(const std::string& name, u32 baseAddr, int timerId);

    int getId() const { return id; }

    bool isEnabled() const { return enabled; }
    void SetEnabled(bool en) { enabled = en; }

    bool isRunning() const { return running; }
    void SetRunning(bool run) { running = run; }

    bool isOverflow() const { return overflow; }
    void clearOverflow() { overflow = false; }

    bool isInterruptPending() const { return interruptPending; }
    void clearInterruptPending() { interruptPending = false; }

private:
    int id;
    bool enabled = false;
    bool running = false;
    bool overflow = false;
    bool interruptPending = false;

    u16 config = 0;
    u32 counter = 0;
    u32 period = 0;
    u32 width = 0;
};

GPTimerImpl::GPTimerImpl(const std::string& name, u32 baseAddr, int timerId)
    : RegisterDevice(name, baseAddr, 0x10) , id(timerId) {
    REG32(TIMER_CONFIG, 0x00);
    FIELD(TIMER_CONFIG, CONFIG, 0, 16, R(config), W(config));

    REG32(TIMER_COUNTER, 0x04);
    FIELD(TIMER_COUNTER, COUNTER, 0, 32, R(counter), W(counter));

    REG32(TIMER_PERIOD, 0x08);
    FIELD(TIMER_PERIOD, PERIOD, 0, 32, R(period), W(period));

    REG32(TIMER_WIDTH, 0x0C);
    FIELD(TIMER_WIDTH, WIDTH, 0, 32, R(width), W(width));
}

GPTimer::GPTimer(u32 baseAddr) : RegisterDevice("GPTimer", baseAddr, 0x90) {
    for (int i = 0; i < 8; ++i) {
        timers[i] = std::make_shared<GPTimerImpl>("GPTimer" + std::to_string(i), baseAddr + i * 0x10, i);
    }

    // TIMER_ENABLE
    REG32(TIMER_ENABLE, 0x80);
    auto enable_read = [this]() -> u32 {
        u8 mask = 0;
        for (int i = 0; i < 8; ++i) {
            if (timers[i]->isEnabled()) {
                mask |= (1 << i);
            }
        }
        return mask;
    };
    auto enable_write = [this](u32 v) {
        u8 mask = static_cast<u8>(v);
        for (int i = 0; i < 8; ++i) {
            if (mask & (1 << i)) {
                timers[i]->SetEnabled(true);
            }
        }
    };
    FIELD(TIMER_ENABLE, ENABLE, 0, 8, enable_read, enable_write);

    // TIMER_DISABLE
    REG32(TIMER_DISABLE, 0x84);
    auto disable_write = [this](u32 v) {
        u8 mask = static_cast<u8>(v);
        for (int i = 0; i < 8; ++i) {
            if (mask & (1 << i)) {
                timers[i]->SetEnabled(false);
            }
        }
    };
    FIELD(TIMER_DISABLE, DISABLE, 0, 8, enable_read, disable_write);

    // TIMER_STATUS at offset 0x08 (32-bit, W1C for interrupt/overflow bits)
    REG32(TIMER_STATUS, 0x88);
    auto status_read = [this]() -> u32 {
        u32 status = 0;
        for (int i = 0; i < 2; i++) {
            int index = i * 4;
            int shift = i * 16;
            status |= timers[index + 3]->isRunning() << (shift + 15);
            status |= timers[index + 2]->isRunning() << (shift + 14);
            status |= timers[index + 1]->isRunning() << (shift + 13);
            status |= timers[index + 0]->isRunning() << (shift + 12);
            status |= timers[index + 3]->isOverflow() << (shift + 7);
            status |= timers[index + 2]->isOverflow() << (shift + 6);
            status |= timers[index + 1]->isOverflow() << (shift + 5);
            status |= timers[index + 0]->isOverflow() << (shift + 4);
            status |= timers[index + 3]->isInterruptPending() << (shift + 3);
            status |= timers[index + 2]->isInterruptPending() << (shift + 2);
            status |= timers[index + 1]->isInterruptPending() << (shift + 1);
            status |= timers[index + 0]->isInterruptPending() << (shift + 0);
        }
        return status;
    };
    auto status_write = [this](u32 v) {
        for (int i = 0; i < 2; i++) {
            int index = i * 4;
            int shift = i * 16;
#define DO_WITH_MASK(bit, ...) do { if (v & (1 << (shift + bit))) { __VA_ARGS__; } } while(0)
            DO_WITH_MASK(15, timers[index + 3]->SetRunning(false));
            DO_WITH_MASK(14, timers[index + 2]->SetRunning(false));
            DO_WITH_MASK(13, timers[index + 1]->SetRunning(false));
            DO_WITH_MASK(12, timers[index + 0]->SetRunning(false));
            DO_WITH_MASK(7, timers[index + 3]->clearOverflow());
            DO_WITH_MASK(6, timers[index + 2]->clearOverflow());
            DO_WITH_MASK(5, timers[index + 1]->clearOverflow());
            DO_WITH_MASK(4, timers[index + 0]->clearOverflow());
            DO_WITH_MASK(3, timers[index + 3]->clearInterruptPending());
            DO_WITH_MASK(2, timers[index + 2]->clearInterruptPending());
            DO_WITH_MASK(1, timers[index + 1]->clearInterruptPending());
            DO_WITH_MASK(0, timers[index + 0]->clearInterruptPending());
#undef DO_WITH_MASK
        }
    };
    FIELD(TIMER_STATUS, STATUS, 0, 32, status_read, status_write);
}

u32 GPTimer::Read32(u32 offset) {
    if (offset < 0x80) {
        int timerIndex = offset / 0x10;
        return timers[timerIndex]->Read32(offset % 0x10);
    }
    return RegisterDevice::Read32(offset);
}

void GPTimer::Write32(u32 offset, u32 value) {
    if (offset < 0x80) {
        int timerIndex = offset / 0x10;
        timers[timerIndex]->Write32(offset % 0x10, value);
        return;
    }
    RegisterDevice::Write32(offset, value);
}