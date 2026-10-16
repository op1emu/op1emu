#include "cpu/time_source.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

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
        // Inside a block, a device sees the time of the packet accessing it.
        uint32_t index = 0;
        time.SetBlockPackets(&index);
        Require(time.Cycles() == 400, "between blocks: the packets retired");
        index = 1;
        Require(time.Cycles() == 400 && time.Packets() == 400, "the block's first packet runs at its start");
        index = 5;
        Require(time.Cycles() == 404 && time.Nanoseconds() == 1010, "the fifth packet runs four cycles later");
        Require(time.Packets() == 400, "Packets() counts retired packets only");
        index = 0;
        time.SetBlockPackets(nullptr);
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
        TimeSource time(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        time.StartWallClock();
        Require(time.Cycles() < 400 * 100000, "wall time starts at StartWallClock, not construction");
        TimeSource guest(true);
        guest.Retire(10);
        guest.StartWallClock();
        Require(guest.Cycles() == 10, "StartWallClock leaves deterministic time alone");
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
        Require(panel.DeadlineNs() == 33334334, "the deadline is when Service() acts next");
        Require(!panel.Service(panel.DeadlineNs() - 1) && panel.Service(panel.DeadlineNs()), "and not before");
    }
    {
        // A chain budget in packets: the first packet count whose guest time
        // reaches a due time, for every rounding of 2.5 ns per packet.
        TimeSource time(true);
        for (uint64_t ns = 0; ns < 2000; ++ns) {
            const uint64_t p = TimeSource::PacketsAt(ns);
            Require(p * 5 / 2 >= ns && (p == 0 || (p - 1) * 5 / 2 < ns), "PacketsAt is the least packet count reaching ns");
        }
        Require(TimeSource::PacketsAt(UINT64_MAX) == UINT64_MAX, "never stays never");
        const auto epoch = time.SystemNow();
        Require(time.DueNs(epoch) == 0 && time.DueNs(epoch - std::chrono::seconds(1)) == 0, "due now");
        time.Retire(TimeSource::PacketsAt(time.DueNs(epoch + std::chrono::nanoseconds(1000000001))));
        Require(time.SystemNow() >= epoch + std::chrono::nanoseconds(1000000001), "SystemNow reaches the due time");
        Require((time.Packets() - 1) * 5 / 2 < 1000000001, "at the first packet that does");
    }
    std::puts("TimeSource and PanelOscillator tests passed");
}
