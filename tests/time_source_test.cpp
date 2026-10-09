#include "cpu/time_source.h"
#include <cstdio>
#include <cstdlib>

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    {
        TimeSource time(true);
        Require(time.Deterministic() && time.Slowdown() == 1, "deterministic mode needs no slowdown");
        Require(time.Cycles() == 0 && time.Nanoseconds() == 0, "guest time starts at zero");
        time.Retire(3);
        time.Retire(397);
        Require(time.Packets() == 400 && time.Cycles() == 400, "one cycle per executed packet");
        Require(time.Nanoseconds() == 1000, "400 MHz: 400 cycles are 1 us");
        const auto epoch = TimeSource::SystemClock::time_point(std::chrono::seconds(1000));
        time.SetEpoch(epoch);
        Require(time.SystemNow() == epoch + std::chrono::microseconds(1), "RTC time is epoch plus guest time");
        TimeSource other(true);
        Require(other.SystemNow() == TimeSource::SystemClock::time_point(
                                         std::chrono::seconds(TimeSource::kDefaultEpochSeconds)),
                "fixed default epoch");
    }
    {
        TimeSource time(false);
        Require(!time.Deterministic() && time.Slowdown() == TimeSource::kWallSlowdown, "wall mode keeps its slowdown");
        time.Retire(1000000);
        Require(time.Packets() == 1000000, "wall mode still counts packets");
        Require(time.Cycles() < 1000000, "wall mode does not take time from packets");
    }
    {
        PanelOscillator panel;
        auto level = panel.Service(0);
        Require(level && *level, "TE pulses low at guest time zero");
        Require(!panel.Service(999), "pulse holds for 1 us");
        level = panel.Service(1000);
        Require(level && !*level, "pulse ends after 1 us");
        Require(!panel.Service(16666666), "no pulse before 1/60 s");
        level = panel.Service(16666667);
        Require(level && *level, "next pulse at 1/60 s, rounded up");
        level = panel.Service(16667667);
        Require(level && !*level, "second pulse ends 1 us later");
        level = panel.Service(33333334);
        Require(level && *level, "third frame at 2/60 s");
    }
    std::puts("TimeSource and PanelOscillator tests passed");
}
