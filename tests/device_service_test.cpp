// A device serviced only when it has work must show the guest exactly what
// the same device shows when it is serviced after every block: each test
// drives two instances with the same inputs, one polled at every step and one
// through its service contract, and compares what the guest can observe.
#include "cpu/rtc.h"
#include "cpu/time_source.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

static void Require(bool ok, const std::string& message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}

// The RTC's registers, as the guest reads them.
static std::vector<u32> RtcRegisters(RTC& rtc) {
    std::vector<u32> out;
    for (u32 offset = 0; offset < 0x18; offset += 4) out.push_back(rtc.Read32(offset));
    return out;
}

static void RtcMatchesPolling(uint32_t seed) {
    TimeSource polledTime(true), scheduledTime(true);
    RTC polled(0xFFC00300, polledTime), scheduled(0xFFC00300, scheduledTime);
    std::vector<int> polledLevels, scheduledLevels;
    polled.BindInterrupt(14, [&](int, int level) { polledLevels.push_back(level); });
    scheduled.BindInterrupt(14, [&](int, int level) { scheduledLevels.push_back(level); });

    std::mt19937 rng(seed);
    uint64_t ticks = 0;
    // The calendar's seconds start at guest time zero, and again at each
    // RTC_STAT write: steps aimed at their boundaries catch a deadline that
    // is off by a packet.
    uint64_t secondsOrigin = 0;
    for (int step = 0; step < 20000; ++step) {
        uint64_t packets;
        if (rng() % 4 == 0) {
            const uint64_t now = polledTime.Nanoseconds();
            const uint64_t boundary = secondsOrigin + ((now - secondsOrigin) / 1000000000u + 1) * 1000000000u;
            const uint64_t target = boundary - 3 + rng() % 7;  // within a packet or two of it
            const uint64_t targetPackets = (target * 2 + 4) / 5;  // first packet count reaching target ns
            packets = targetPackets > polledTime.Packets() ? targetPackets - polledTime.Packets() : 1;
        } else {
            // Mostly short blocks, sometimes a long stretch of guest time.
            packets = rng() % 8 == 0 ? rng() % 800000000u : rng() % 4000 + 1;
        }
        polledTime.Retire(packets);
        scheduledTime.Retire(packets);
        if (rng() % 64 == 0) {
            static const u32 offsets[] = {0x00, 0x04, 0x08, 0x0C, 0x10, 0x14};
            const u32 offset = offsets[rng() % 6];
            u32 value = rng();
            if (offset == 0x14) value &= 1;
            if (offset == 0x04) value &= 0x807F;
            polled.Write32(offset, value);
            scheduled.Write32(offset, value);
            if (offset == 0x00) secondsOrigin = polledTime.Nanoseconds();
        }
        polled.Tick();
        if (scheduled.NeedsTick()) {
            scheduled.Tick();
            ++ticks;
        } else {
            scheduled.ReplayInterrupt();
        }
        Require(RtcRegisters(polled) == RtcRegisters(scheduled),
                "seed " + std::to_string(seed) + ": registers differ at step " + std::to_string(step));
        Require(polledLevels == scheduledLevels,
                "seed " + std::to_string(seed) + ": published levels differ at step " + std::to_string(step));
    }
    Require(ticks < 20000, "the scheduled RTC must skip calls with nothing to do");
}

int main() {
    for (uint32_t seed = 1; seed <= 20; ++seed) RtcMatchesPolling(seed);
    std::puts("device-service-test: ok");
}
