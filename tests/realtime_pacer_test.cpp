#include "cpu/realtime_pacer.h"
#include <cstdio>
#include <cstdlib>

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    constexpr uint64_t kMs = 1000000;
    {
        RealtimePacer pacer;
        Require(!pacer.Started(), "a pacer starts at its first Sync");
        pacer.Sync(5 * kMs, 1000 * kMs);
        Require(pacer.Started(), "and is started then");
        Require(pacer.GuestNsAt(1000 * kMs) == 5 * kMs, "guest and wall time line up at the start");
        Require(pacer.GuestNsAt(1003 * kMs) == 8 * kMs, "one guest ns per wall ns");
        Require(pacer.GuestNsAt(999 * kMs) == 5 * kMs, "no guest time before the start");
        pacer.Sync(50 * kMs, 1001 * kMs);
        Require(pacer.GuestNsAt(1001 * kMs) == 6 * kMs, "a guest ahead of the wall clock moves nothing");
    }
    {
        RealtimePacer pacer;
        pacer.Sync(0, 0);
        pacer.Sync(0, RealtimePacer::kMaxLagNs);
        Require(pacer.GuestNsAt(RealtimePacer::kMaxLagNs) == RealtimePacer::kMaxLagNs,
                "a guest kMaxLagNs behind catches up on its own");
        pacer.Sync(0, RealtimePacer::kMaxLagNs + 300 * kMs);
        Require(pacer.GuestNsAt(RealtimePacer::kMaxLagNs + 300 * kMs) == RealtimePacer::kMaxLagNs,
                "a guest further behind is let off the rest");
        Require(pacer.GuestNsAt(RealtimePacer::kMaxLagNs + 301 * kMs) == RealtimePacer::kMaxLagNs + kMs,
                "and goes on at one guest ns per wall ns");
        uint64_t last = 0;
        for (uint64_t wall = 0; wall < 600 * kMs; wall += 7 * kMs) {
            const uint64_t at = pacer.GuestNsAt(wall);
            Require(at >= last, "the guest time a wall time stands for never goes back");
            last = at;
        }
    }
    std::puts("RealtimePacer tests passed");
    return 0;
}
