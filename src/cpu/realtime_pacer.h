#pragma once

#include <cstdint>

// Real-time mode: which guest time the host's clock stands for, so that the
// CPU keeps guest time from running ahead of it (BlackFinCpu::Pace). Times
// are nanoseconds, the wall ones on any steady clock.
//
// A guest behind the clock catches up by running flat out, but by no more
// than kMaxLagNs: after a host stall (JIT compilation, a descheduled thread)
// the rest is let off, or the guest would see the stall's time rush by, as
// the GP timers' kMaxCatchupTicks drops a long gap's ticks.
class RealtimePacer {
public:
    static constexpr uint64_t kMaxLagNs = 100000000;
    // How far guest time may run ahead of the clock before the CPU sleeps,
    // and then sleeps until the clock is there: about one sleep per kAheadNs.
    static constexpr uint64_t kAheadNs = 1000000;
    // The longest sleep at once, so that work from another thread (a key,
    // USB) is seen that soon.
    static constexpr uint64_t kSliceNs = 1000000;

    bool Started() const { return started_; }

    // The guest is at guestNs at wall time wallNs: the first call lines the
    // two up, later ones let a guest more than kMaxLagNs behind off the rest.
    void Sync(uint64_t guestNs, uint64_t wallNs) {
        if (!started_) {
            started_ = true;
            startGuest_ = guestNs;
            startWall_ = wallNs;
            return;
        }
        const uint64_t due = GuestNsAt(wallNs);
        if (due > guestNs + kMaxLagNs) startWall_ += due - guestNs - kMaxLagNs;
    }

    // The guest time due at wall time wallNs.
    uint64_t GuestNsAt(uint64_t wallNs) const {
        return wallNs <= startWall_ ? startGuest_ : startGuest_ + (wallNs - startWall_);
    }

private:
    bool started_ = false;
    uint64_t startGuest_ = 0;
    uint64_t startWall_ = 0;
};
