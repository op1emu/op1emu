#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// The host side of the audio path. The SPORT makes frames at 44.1 kHz of
// guest time, which the pacer holds to the host's monotonic clock; the sound
// card takes them at 44.1 kHz of its own crystal. These keep the ring between
// them at a target level: the card starts (and, after running dry, starts
// again) only once the ring holds it, and the producer resamples by up to 1%
// so that the two clocks' drift does not move it.

// The level the ring is kept at for the card's requests of up to request
// frames: base, or more when a request alone would take most of it.
inline uint32_t TargetLevel(uint32_t base, uint32_t request) {
    return std::max(base, request + base / 2);
}

// The sound card's side (its data callback): which of the ring's frames a
// request plays. Nothing until the ring holds the target (TargetLevel); a
// request it cannot fill plays what there is and waits for the target again,
// so a late producer costs one gap, not a run of short ones. A producer
// catching up after it was late can fill the ring far past the target: above
// twice the target the oldest frames are skipped down to it, one jump instead
// of seconds at the servo's other pitch.
class PlaybackGate {
public:
    explicit PlaybackGate(uint32_t baseFrames) : base_(baseFrames) {}

    struct Frames {
        uint32_t skip = 0, play = 0;
    };
    Frames Take(uint32_t available, uint32_t requested) {
        const uint32_t target = TargetLevel(base_, requested);
        if (!playing_) {
            if (available < target) return {};
            playing_ = true;
        }
        Frames frames;
        if (available > 2 * target) {
            frames.skip = available - target;
            available = target;
        }
        if (available >= requested) {
            frames.play = requested;
        } else {
            playing_ = false;
            frames.play = available;
        }
        return frames;
    }
    bool Playing() const { return playing_; }

private:
    uint32_t base_;
    bool playing_ = false;
};

// The producer's side: the input frames per output frame that bring the
// ring's level to the target. The level comes in steps of the card's
// requests, so the servo follows its average (time constant smoothFrames).
// Its error, relative to the target, changes the rate by gain times as much,
// at most by gain; and the error's integral (time constant integralFrames)
// takes over the clocks' drift, at most maxDrift, which then leaves the level
// at the target. The sum takes an error up to a quarter of the target: larger
// ones are a late producer's gap and catch-up, not the drift.
class LevelServo {
public:
    LevelServo(double targetFrames, double gain, double maxDrift, double smoothFrames, double integralFrames)
        : target_(targetFrames), gain_(gain), maxDrift_(maxDrift), smooth_(smoothFrames),
          integral_(integralFrames) {}

    // level: the ring's frames now; frames: the input frames since the last call.
    double Update(double level, size_t frames) {
        if (!started_) {
            average_ = level;
            started_ = true;
        } else {
            average_ += (level - average_) * std::min(1.0, double(frames) / smooth_);
        }
        const double error = (average_ - target_) / target_;
        const double summed = std::clamp(error, -kSummedError, kSummedError);
        drift_ = std::clamp(drift_ + gain_ * summed * double(frames) / integral_, -maxDrift_, maxDrift_);
        ratio_ = 1.0 + std::clamp(gain_ * error, -gain_, gain_) + drift_;
        return ratio_;
    }
    void SetTarget(double targetFrames) { target_ = targetFrames; }
    double Ratio() const { return ratio_; }

private:
    static constexpr double kSummedError = 0.25;
    double target_, gain_, maxDrift_, smooth_, integral_;
    double average_ = 0.0;
    double drift_ = 0.0;
    double ratio_ = 1.0;
    bool started_ = false;
};

// A stereo stream at ratio input frames per output frame, across calls, by
// 4-point Hermite interpolation: an output frame lies between the input
// frames two and one before the current one, and at ratio 1 is the input
// delayed by two frames. (Linear interpolation, with the phase sweeping through
// at the drift's rate, would make the highs beat: -2.4 dB at 10 kHz.)
class StreamResampler {
public:
    // in: frames stereo frames (L, R), out: appended.
    void Push(const float* in, size_t frames, double ratio, std::vector<float>& out) {
        for (size_t i = 0; i < frames; ++i) {
            const float* next = in + 2 * i;
            while (phase_ < 1.0) {
                const float t = float(phase_);
                for (int c = 0; c < 2; ++c) {
                    const float ym1 = history_[0][c], y0 = history_[1][c], y1 = history_[2][c], y2 = next[c];
                    const float c1 = 0.5f * (y1 - ym1);
                    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
                    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
                    out.push_back(((c3 * t + c2) * t + c1) * t + y0);
                }
                phase_ += ratio;
            }
            phase_ -= 1.0;
            for (int c = 0; c < 2; ++c) {
                history_[0][c] = history_[1][c];
                history_[1][c] = history_[2][c];
                history_[2][c] = next[c];
            }
        }
    }

private:
    double phase_ = 0.0;
    float history_[3][2] = {};
};
