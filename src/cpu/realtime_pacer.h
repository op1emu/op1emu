#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Real-time mode: which guest time the host's clock stands for, so that the
// CPU keeps guest time from running ahead of it (BlackFinCpu::Pace). Times
// are nanoseconds, the wall ones on any steady clock.
//
// A guest behind the clock catches up by running flat out, but by no more
// than kMaxLagNs: after a host stall (JIT compilation, a descheduled thread)
// the rest is let off, or the guest would see the stall's time rush by, as
// the GP timers' kMaxCatchupTicks drops a long gap's ticks.
//
// Guest time goes at the rate SetRate gives, 1 by default: the sound card's
// against the host's clock (AudioOutput::GuestRate), so that the SPORT makes
// frames as fast as the card plays them, as the codec clocks it on the
// device.
class RealtimePacer {
public:
    static constexpr uint64_t kMaxLagNs = 100000000;
    // How far guest time may run ahead of the clock before the CPU sleeps,
    // and then sleeps until the clock is there: about one sleep per kAheadNs.
    static constexpr uint64_t kAheadNs = 1000000;
    // The longest sleep at once, so that work from another thread (a key,
    // USB) is seen that soon.
    static constexpr uint64_t kSliceNs = 1000000;

    // Rates are kept to a millionth, at most kMaxRatePpm off 1.
    static constexpr int64_t kMaxRatePpm = 10000;

    bool Started() const { return started_; }

    // From wall time wallNs on, guest ns per wall ns: the guest time due then
    // stays, and goes on at the new rate.
    void SetRate(double rate, uint64_t wallNs) {
        const int64_t ppm = std::clamp<int64_t>(std::llround((rate - 1.0) * 1e6), -kMaxRatePpm, kMaxRatePpm);
        if (ppm == ratePpm_) return;
        if (started_ && wallNs > startWall_) {
            startGuest_ = GuestNsAt(wallNs);
            startWall_ = wallNs;
        }
        ratePpm_ = ppm;
    }
    int64_t RatePpm() const { return ratePpm_; }

    // The guest is at guestNs at wall time wallNs: the first call lines the
    // two up, later ones let a guest more than kMaxLagNs behind off the rest.
    void Sync(uint64_t guestNs, uint64_t wallNs) {
        if (!started_) {
            started_ = true;
            startGuest_ = guestNs;
            startWall_ = wallNs;
            return;
        }
        if (GuestNsAt(wallNs) > guestNs + kMaxLagNs) {
            startGuest_ = guestNs + kMaxLagNs;
            startWall_ = wallNs;
        }
    }

    // The guest time due at wall time wallNs.
    uint64_t GuestNsAt(uint64_t wallNs) const {
        if (wallNs <= startWall_) return startGuest_;
        const uint64_t wall = wallNs - startWall_;
        // wall * ratePpm_ / 1e6 without overflow, rounded to zero: never back.
        const int64_t off = int64_t(wall / 1000000) * ratePpm_ + int64_t(wall % 1000000) * ratePpm_ / 1000000;
        return startGuest_ + uint64_t(int64_t(wall) + off);
    }

private:
    bool started_ = false;
    uint64_t startGuest_ = 0;
    uint64_t startWall_ = 0;
    int64_t ratePpm_ = 0;
};
