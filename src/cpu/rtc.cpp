#include "rtc.h"

static constexpr int RTC_DAY_BITS_OFF  = 17;
static constexpr int RTC_HOUR_BITS_OFF = 12;
static constexpr int RTC_MIN_BITS_OFF  = 6;
static constexpr int RTC_SEC_BITS_OFF  = 0;

RTC::RTC(u32 baseAddr) : RegisterDevice("RTC", baseAddr, 0x18) {
    REG32(RTC_STAT, 0x00);
    FIELD(RTC_STAT, RTC_STAT, 0, 32, R(GetCurrentStat()), [this](u32 v) {
        writePending = true;
        statShadow = v;
        baseTime = std::chrono::system_clock::now();
        lastStat = statShadow;
    });

    REG32(RTC_ICTL, 0x04);
    FIELD(RTC_ICTL, STOPWATCH, 0, 1, R(stopwatchIntEnabled), W(stopwatchIntEnabled));
    FIELD(RTC_ICTL, ALARM, 1, 1, R(alarmIntEnabled), W(alarmIntEnabled));
    FIELD(RTC_ICTL, SECONDS, 2, 1, R(secondsIntEnabled), W(secondsIntEnabled));
    FIELD(RTC_ICTL, MINUTES, 3, 1, R(minutesIntEnabled), W(minutesIntEnabled));
    FIELD(RTC_ICTL, HOURS, 4, 1, R(hoursIntEnabled), W(hoursIntEnabled));
    FIELD(RTC_ICTL, HOURS24, 5, 1, R(hours24IntEnabled), W(hours24IntEnabled));
    FIELD(RTC_ICTL, DAYALARM, 6, 1, R(dayAlarmIntEnabled), W(dayAlarmIntEnabled));
    FIELD(RTC_ICTL, WRITE_COMPLETE, 15, 1, R(writeCompleteIntEnabled), W(writeCompleteIntEnabled));

    REG32(RTC_ISTAT, 0x08);
    FIELD(RTC_ISTAT, STOPWATCH, 0, 1, R(stopwatchEvent), W1C(stopwatchEvent));
    FIELD(RTC_ISTAT, ALARM, 1, 1, R(alarmEvent), W1C(alarmEvent));
    FIELD(RTC_ISTAT, SEC, 2, 1, R(secondsEvent), W1C(secondsEvent));
    FIELD(RTC_ISTAT, MIN, 3, 1, R(minutesEvent), W1C(minutesEvent));
    FIELD(RTC_ISTAT, HOUR, 4, 1, R(hoursEvent), W1C(hoursEvent));
    FIELD(RTC_ISTAT, HOUR24, 5, 1, R(hours24Event), W1C(hours24Event));
    FIELD(RTC_ISTAT, DAYALARM, 6, 1, R(dayAlarmEvent), W1C(dayAlarmEvent));
    FIELD(RTC_ISTAT, WRITE_PENDING, 14, 1, R(writePending), N());
    FIELD(RTC_ISTAT, WRITE_COMPLETE, 15, 1, R(writeComplete), W1C(writeComplete));

    REG32(RTC_SWCNT, 0x0C);
    FIELD(RTC_SWCNT, COUNT, 0, 16, R(stopwatchCount), [this](u32 v) {
        writePending = true;
        stopwatchCount = v;
    });

    REG32(RTC_ALARM, 0x10);
    FIELD(RTC_ALARM, VAL, 0, 32, R(alarm), [this](u32 v) {
        writePending = true;
        alarm = v;
    });

    REG32(RTC_PREN, 0x14);
    FIELD(RTC_PREN, PREN, 0, 1, R(prescalerEnabled), [this](u32 v) {
        writePending = true;
        prescalerEnabled = (v != 0);
    });

    baseTime = {};
    lastStat = GetCurrentStat();
}

static u32 TimeToBlackfin(std::chrono::system_clock::time_point timePoint) {
    auto duration = timePoint.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();

    u32 sec  = (seconds % 60);
    u32 min  = (seconds % (60 * 60)) / 60;
    u32 hour = (seconds % (60 * 60 * 24)) / (60 * 60);
    u32 days = (seconds / (60 * 60 * 24));
    return (sec  << RTC_SEC_BITS_OFF) |
           (min  << RTC_MIN_BITS_OFF) |
           (hour << RTC_HOUR_BITS_OFF) |
           (days << RTC_DAY_BITS_OFF);
}

static std::tuple<u32, u32, u32, u32> GetBlackfinTime(u32 rtcBfin) {
    u32 sec  = (rtcBfin >> RTC_SEC_BITS_OFF)  & 0x003F;
    u32 min  = (rtcBfin >> RTC_MIN_BITS_OFF)  & 0x003F;
    u32 hour = (rtcBfin >> RTC_HOUR_BITS_OFF) & 0x001F;
    u32 days = (rtcBfin >> RTC_DAY_BITS_OFF)  & 0x7FFF;
    return {days, hour, min, sec};
}

static std::chrono::system_clock::time_point BlackfinToTime(u32 rtcBfin) {
    auto&& [days, hour, min, sec] = GetBlackfinTime(rtcBfin);
    auto totalSeconds = sec + (min * 60) + (hour * 60 * 60) + (days * 60 * 60 * 24);
    return std::chrono::system_clock::time_point(std::chrono::seconds(totalSeconds));
}

u32 RTC::GetCurrentStat() {
    auto now = std::chrono::system_clock::now();
    return TimeToBlackfin(now - baseTime + BlackfinToTime(statShadow));
}

void RTC::UpdateInterrupts() {
    u32 ictl = Read32(0x04);
    u32 istat = Read32(0x08);
    if (ictl & istat != 0) {
        TriggerInterrupt(1);
    } else {
        TriggerInterrupt(0);
    }
}

void RTC::Tick() {
    // Handle write pending completion
    if (writePending) {
        writePending = false;
        writeComplete = true;
    }

    // Check for second tick
    u32 currentStat = GetCurrentStat();
    auto currentStatTime = BlackfinToTime(currentStat);
    auto secondsSinceLast = std::chrono::duration_cast<std::chrono::seconds>(currentStatTime - BlackfinToTime(lastStat)).count();

    // No prescaler not supported, so we assume 1Hz tick
    if (prescalerEnabled && secondsSinceLast >= 1) {
        secondsEvent = true;

        u32 currentStat = GetCurrentStat();
        auto&& [days, hour, min, sec] = GetBlackfinTime(currentStat);
        if (sec == 0) {
            minutesEvent = true;
            if (min == 0) {
                hoursEvent = true;
                if (hour == 0) {
                    hours24Event = true;
                }
            }
        }

        if (alarm != 0) {
            auto&& [alarmDays, alarmHour, alarmMin, alarmSec] = GetBlackfinTime(alarm);
            // Check if alarm matches (ignoring day for RTC_ISTAT_ALARM)
            if (alarmHour == hour && alarmMin == min && alarmSec == sec) {
                alarmEvent = true;
                if (alarmDays == days) {
                    dayAlarmEvent = true;
                }
            }
        }

        // Decrement stopwatch counter
        if (stopwatchCount > 0) {
            stopwatchCount--;
            if (stopwatchCount == 0) {
                stopwatchEvent = true;
            }
        }
    }

    lastStat = currentStat;
    UpdateInterrupts();
}

void RTC::ProcessWithInterrupt(int ivg) {
    Tick();
}