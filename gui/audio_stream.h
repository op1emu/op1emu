#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// The host side of the audio path. The SPORT makes frames at 44.1 kHz of
// guest time, which the pacer holds to the host's monotonic clock at a rate;
// the sound card takes them at 44.1 kHz of its own crystal. These keep the
// ring between them at a target level: the card starts (and, after running
// dry, starts again) only once the ring holds it, and the pacer's rate is
// the card's, so that the two clocks' drift does not move it -- the card
// clocks the SPORT, as the codec does on the device, and its frames play as
// they are.

// The latency asked for in all (--audio-buffer), from the CPU thread's
// write to the card's output: a third, rounded up, to the card (its period;
// see MiniaudioOutput) and the rest to the ring, which the producer running
// late draws on. (DuckStation's defaults give the card 2/7, Dolphin's 1/5.)
struct LatencySplit {
    unsigned cardMs, ringMs;
};
constexpr LatencySplit SplitLatencyMs(unsigned totalMs) {
    const unsigned card = (totalMs + 2) / 3;
    return {card, totalMs - card};
}

// The level the ring is kept at for the card's requests of up to request
// frames: base, or more when a request alone would take most of it -- for
// requests of up to kMaxRequestFrames, so that a backend asking for its
// whole buffer at once does not raise it past what the ring holds.
constexpr uint32_t kMaxRequestFrames = 4096;
constexpr uint32_t TargetLevel(uint32_t base, uint32_t request) {
    return std::max(base, std::min(request, kMaxRequestFrames) + base / 2);
}

// The sound card's side (its data callback): which of the ring's frames a
// request plays. Nothing until the ring holds the target (TargetLevel, for
// the largest request of the last one to two windowFrames of the card's: a
// backend's first, longer, one raises it only for that long); a request it
// cannot fill plays what there is and waits for the target again, so a late
// producer costs one gap, not a run of short ones. A producer catching up
// after it was late can fill the ring past the target: on starting again
// after a gap, and above twice the target, the oldest frames are skipped down
// to where the servo holds the level -- between requests it rises by one, so
// to the target and half a request before one -- a jump instead of seconds
// at the servo's other rate; never below the request, which a skip must not
// leave short. The servo's target is the gate's (Target()), or the two would
// hold different levels and the gate skip what the servo makes up.
class PlaybackGate {
public:
    PlaybackGate(uint32_t baseFrames, uint32_t windowFrames) : base_(baseFrames), window_(windowFrames) {}

    struct Frames {
        uint32_t skip = 0, play = 0;
    };
    Frames Take(uint32_t available, uint32_t requested) {
        Remember(requested);
        const uint32_t target = Target();
        bool starting = false;
        if (!playing_) {
            if (available < target) return {};
            playing_ = starting = true;
        }
        Frames frames;
        const uint32_t held = std::max(target + requested / 2, requested);
        if (available > (starting ? held : std::max(held, 2 * target))) {
            frames.skip = available - held;
            available = held;
        }
        if (available >= requested) {
            frames.play = requested;
        } else {
            playing_ = false;
            frames.play = available;
        }
        return frames;
    }
    uint32_t Target() const { return TargetLevel(base_, std::max(current_, previous_)); }
    bool Playing() const { return playing_; }

private:
    // The largest request of this window and the last.
    void Remember(uint32_t requested) {
        current_ = std::max(current_, requested);
        seen_ += requested;
        if (seen_ >= window_) {
            previous_ = current_;
            current_ = seen_ = 0;
        }
    }

    uint32_t base_, window_;
    uint32_t current_ = 0, previous_ = 0;
    uint64_t seen_ = 0;
    bool playing_ = false;
};

// The producer's side: the pacer's rate (guest ns per wall ns) that brings
// the ring's level to the target. The level comes in steps of the card's
// requests, so the servo follows its average (time constant smoothFrames).
// Its error, relative to the target, slows guest time gain times as much, by
// at most maxPull; and the error's integral (time constant integralFrames)
// takes over the clocks' drift, at most maxDrift, which then leaves the level
// at the target. The sum takes an error up to a quarter of the target: larger
// ones are a late producer's gap and catch-up, not the drift. A rate off 1
// is a pitch off the guest's (0.1%: 1.7 cents), so the bounds are a few
// times the drift of a card's crystal.
class LevelServo {
public:
    LevelServo(double targetFrames, double gain, double maxPull, double maxDrift, double smoothFrames,
               double integralFrames)
        : target_(targetFrames), gain_(gain), maxPull_(maxPull), maxDrift_(maxDrift), smooth_(smoothFrames),
          integral_(integralFrames) {}

    // level: the ring's frames now; frames: the frames written since the last
    // call. Returns Rate().
    double Update(double level, size_t frames) {
        if (!started_) {
            average_ = level;
            started_ = true;
        } else {
            average_ += (level - average_) * std::min(1.0, double(frames) / smooth_);
        }
        const double error = (average_ - target_) / target_;
        const double summed = std::clamp(error, -kSummedError, kSummedError);
        drift_ = std::clamp(drift_ - gain_ * summed * double(frames) / integral_, -maxDrift_, maxDrift_);
        rate_ = 1.0 - std::clamp(gain_ * error, -maxPull_, maxPull_) + drift_;
        return rate_;
    }
    void SetTarget(double targetFrames) { target_ = targetFrames; }
    double Rate() const { return rate_; }
    // The clocks' drift alone: the rate with the level at the target.
    double Drift() const { return 1.0 + drift_; }

private:
    static constexpr double kSummedError = 0.25;
    double target_, gain_, maxPull_, maxDrift_, smooth_, integral_;
    double average_ = 0.0;
    double drift_ = 0.0;
    double rate_ = 1.0;
    bool started_ = false;
};

// Whether the card plays what the CPU thread writes, so that the ring's
// level is the card's to follow. Not while the gate waits for the target,
// the SPORT is off (nothing written for kStaleNs) or the card stopped asking
// (suspended, unplugged).
inline bool FollowsCard(bool playing, uint64_t sinceWriteNs, uint64_t sinceCallbackNs) {
    constexpr uint64_t kStaleNs = 100000000;
    return playing && sinceWriteNs < kStaleNs && sinceCallbackNs < kStaleNs;
}

// The pacer's rate: the servo's while it follows the card, which only then
// updates it; else the drift learned, which it picks up from when the card
// comes back.
inline double PaceRate(const LevelServo& servo, bool followsCard) {
    return followsCard ? servo.Rate() : servo.Drift();
}
