#pragma once

#include "io.h"
#include <chrono>

class RTC : public RegisterDevice {
public:
    RTC(u32 baseAddr);

    void Tick();  // Called periodically to update RTC state
    void ProcessWithInterrupt(int ivg) override;

private:
    void UpdateInterrupts();
    u32 GetCurrentStat();

    bool stopwatchIntEnabled = false;
    bool alarmIntEnabled = false;
    bool secondsIntEnabled = false;
    bool minutesIntEnabled = false;
    bool hoursIntEnabled = false;
    bool hours24IntEnabled = false;
    bool dayAlarmIntEnabled = false;
    bool writeCompleteIntEnabled = false;

    bool stopwatchEvent = false;
    bool alarmEvent = false;
    bool secondsEvent = false;
    bool minutesEvent = false;
    bool hoursEvent = false;
    bool hours24Event = false;
    bool dayAlarmEvent = false;
    bool writePending = false;
    bool writeComplete = false;

    bool prescalerEnabled = false;

    u16 stopwatchCount = 0;
    u32 alarm = 0;

    u32 statShadow = 0;
    u32 lastStat = 0;
    std::chrono::system_clock::time_point baseTime;
};