#include "gptimer.h"

enum GPTimerMode {
    GPTIMER_MODE_RESET = 0x0,
    GPTIMER_MODE_PWM_OUT = 0x1,
    GPTIMER_MODE_WDTH_CAP = 0x2,
    GPTIMER_MODE_EXT_CLK = 0x3,
};

enum GPTimerPWMOutClock {
    GPTimer_PWM_OUT_CLOCK_FROM_TACLK = 0,
    GPTimer_PWM_OUT_CLOCK_FROM_TMRCLK = 1,
};

enum GPTimerWidthCaptureMode {
    GPTimer_WIDTH_CAPTURE_TMR = 0,
    GPTimer_WIDTH_CAPTURE_TACI = 1,
};

enum GPTimerClockSelect {
    GPTimer_CLOCK_SELECT_SCLK = 0,
    GPTimer_CLOCK_SELECT_PWM_CLK = 1,
};

enum GPTimerErrorType {
    GPTimer_ERROR_TYPE_NONE = 0,
    GPTimer_ERROR_TYPE_OVERFLOW = 1,
};

class GPTimerImpl : public RegisterDevice {
public:
    GPTimerImpl(const std::string& name, u32 baseAddr, int timerId);

    int getId() const { return id; }

    bool isEnabled() const { return enabled; }
    void SetEnabled(bool en);

    bool isRunning() const { return running; }
    void SetRunning(bool run);

    bool isOverflow() const { return overflow; }
    void clearOverflow() { overflow = false; errorType = GPTimer_ERROR_TYPE_NONE; }

    bool isInterruptPending() const { return interruptPending; }
    void clearInterruptPending();

    void Tick(GPTimerClockType clockType);

private:
    void startTimer();
    void stopTimer();
    void updateTimerInterval();

    int id;
    bool enabled = false;
    bool running = false;
    bool overflow = false;
    bool interruptPending = false;

    u8 mode = GPTIMER_MODE_RESET;
    bool positiveActionPulse = false;
    bool countToEndOfPeriod = false;
    bool interruptEnabled = false;
    u8 timerInputSelect = 0;
    u8 timerClockSelect = 0;
    u8 errorType = 0;

    u32 scale = 0;
    u32 counter = 1;
    u32 period = 0;
    u32 width = 0;
    u32 bufferedPeriod = 0;
    u32 bufferedWidth = 0;

    GPTimerClockType clockType = GPTimerClockTypeSCLK;
};

GPTimerImpl::GPTimerImpl(const std::string& name, u32 baseAddr, int timerId)
    : RegisterDevice(name, baseAddr, 0x10), id(timerId) {

    REG32(TIMER_CONFIG, 0x00);
    FIELD(TIMER_CONFIG, TMODE, 0, 2, R(mode), W(mode));
    FIELD(TIMER_CONFIG, PULSE_HI, 2, 1, R(positiveActionPulse ? 1 : 0), W(positiveActionPulse));
    FIELD(TIMER_CONFIG, PERIOD_CNT, 3, 1, R(countToEndOfPeriod ? 1 : 0), W(countToEndOfPeriod));
    FIELD(TIMER_CONFIG, IRQ_ENA, 4, 1, R(interruptEnabled ? 1 : 0), W(interruptEnabled));
    FIELD(TIMER_CONFIG, TIN_SEL, 5, 1, R(timerInputSelect), W(timerInputSelect));
    FIELD(TIMER_CONFIG, CLK_SEL, 7, 1, R(timerClockSelect), W(timerClockSelect));
    FIELD(TIMER_CONFIG, ERR_TYP, 14, 2, R(errorType), N());

    REG32(TIMER_COUNTER, 0x04);
    FIELD(TIMER_COUNTER, COUNTER, 0, 32, R(counter), N());

    REG32(TIMER_PERIOD, 0x08);
    FIELD(TIMER_PERIOD, PERIOD, 0, 32, R(period), W(bufferedPeriod));

    REG32(TIMER_WIDTH, 0x0C);
    FIELD(TIMER_WIDTH, WIDTH, 0, 32, R(width), W(bufferedWidth));
}

void GPTimerImpl::SetEnabled(bool en) {
    if (!enabled && en) {
        startTimer();
    }
    enabled = en;
    // Stopping is handled in Tick
}

void GPTimerImpl::SetRunning(bool run) {
    running = run;
    // Stop immediately
    if (!running) {
        stopTimer();
    }
}

void GPTimerImpl::startTimer() {
    counter = timerClockSelect == GPTimer_CLOCK_SELECT_PWM_CLK ? 0 : 1;
    if (timerClockSelect == GPTimer_CLOCK_SELECT_SCLK) {
        clockType = GPTimerClockTypeSCLK;
    } else if (timerInputSelect == GPTimer_PWM_OUT_CLOCK_FROM_TACLK) {
        clockType = GPTimerClockTypeTACLK;
    } else {
        clockType = GPTimerClockTypeTMRCLK;
    }
    period = bufferedPeriod;
    width = bufferedWidth;
    running = true;
}

void GPTimerImpl::stopTimer() {
    running = false;
    // Load buffered values if any
    period = bufferedPeriod;
    width = bufferedWidth;
}

void GPTimerImpl::clearInterruptPending() {
    interruptPending = false;
    TriggerInterrupt(0);
}

void GPTimerImpl::Tick(GPTimerClockType clock) {
    if (!running || clock != clockType) return;

    // Only support PWM output mode for now

    // Increment counter
    counter++;

    // Check for overflow
    if (counter == 0xFFFFFFFF) {
        overflow = true;
        errorType = GPTimer_ERROR_TYPE_OVERFLOW;
        if (interruptEnabled) {
            TriggerInterrupt(1);
        }
    }

    if (counter == period && countToEndOfPeriod) {
        interruptPending = true;
        if (interruptEnabled) {
            TriggerInterrupt(1);
        }
        stopTimer();
        if (enabled) {
            counter = 0;
            running = true;
        }
    } else if (counter == width && !countToEndOfPeriod) {
        interruptPending = true;
        if (interruptEnabled) {
            TriggerInterrupt(1);
        }
        enabled = false;
        stopTimer();
    }
}

GPTimer::GPTimer(u32 baseAddr) : RegisterDevice("GPTimer", baseAddr, 0x90) {
    for (int i = 0; i < 8; ++i) {
        timers[i] = std::make_shared<GPTimerImpl>("GPTimer" + std::to_string(i), baseAddr + i * 0x10, i);
    }

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

GPTimer::~GPTimer() = default;

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

void GPTimer::BindInterrupt(int id, int q, InterruptHandler callback) {
    if (id < 0 || id >= timers.size()) return;
    timers[id]->BindInterrupt(q, callback);
}

void GPTimer::Tick(GPTimerClockType clockType) {
    for (auto& timer : timers) {
        timer->Tick(clockType);
    }
}