#include "audio_stream.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

static void Split() {
    auto split = [](unsigned total, unsigned card, unsigned ring) {
        const LatencySplit s = SplitLatencyMs(total);
        return s.cardMs == card && s.ringMs == ring;
    };
    check(split(26, 9, 17), "the default: the card and ring measured on a desktop");
    check(split(9, 3, 6), "a third to the card");
    check(split(10, 4, 6), "rounded up");
    check(split(200, 67, 133), "the most");
}

static void Gate() {
    PlaybackGate gate(100);
    auto take = [&](uint32_t available, uint32_t requested, uint32_t skip, uint32_t play) {
        const PlaybackGate::Frames frames = gate.Take(available, requested);
        return frames.skip == skip && frames.play == play;
    };
    check(take(99, 50, 0, 0), "waits for its level");
    check(take(100, 50, 0, 50), "starts at its level");
    check(take(30, 50, 0, 30), "plays what a short ring has");
    check(!gate.Playing(), "and waits again");
    check(take(60, 50, 0, 0), "below its level after running dry");
    check(take(150, 50, 0, 50), "starts again");
    check(take(50, 50, 0, 50), "an exact request is no underrun");
    check(gate.Playing(), "still playing");
    check(take(200, 50, 0, 50), "keeps a full ring up to twice its level");
    check(take(201, 50, 101, 50), "skips down to its level above it");
    check(take(1000, 50, 900, 50), "however far above");

    PlaybackGate large(100);
    check(TargetLevel(100, 150) == 200, "a large request raises the level");
    check(large.Take(199, 150).play == 0, "which the gate waits for");
    check(large.Take(200, 150).play == 150, "and then plays");
    check(large.Take(200, 150).play == 150 && large.Playing(), "a request at a time");
}

static void Resampler() {
    std::vector<float> in;
    for (int i = 0; i < 1000; ++i) { in.push_back(float(i)); in.push_back(-float(i)); }
    std::vector<float> out;
    StreamResampler same;
    for (int i = 0; i < 1000; i += 7) same.Push(in.data() + 2 * i, std::min(7, 1000 - i), 1.0, out);
    check(out.size() == in.size(), "ratio 1: a frame for a frame");
    check(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f && out[3] == 0.0f, "starting from silence");
    for (int i = 2; i < 1000; ++i)
        check(out[2 * i] == in[2 * (i - 2)] && out[2 * i + 1] == in[2 * (i - 2) + 1], "ratio 1: the input two frames later");

    for (double ratio : {1.01, 0.99}) {
        out.clear();
        StreamResampler r;
        for (int i = 0; i < 1000; i += 3) r.Push(in.data() + 2 * i, std::min(3, 1000 - i), ratio, out);
        const double expected = 1000 / ratio;
        check(std::fabs(double(out.size() / 2) - expected) <= 1.0, "frames out = frames in / ratio");
        // A ramp stays a ramp: output frame k is input position k * ratio - 2.
        for (size_t k = 3; k < out.size() / 2; ++k) {
            const double pos = double(k) * ratio - 2.0;
            check(std::fabs(out[2 * k] - pos) < 1e-3 && std::fabs(out[2 * k + 1] + pos) < 1e-3,
                  "interpolates across calls");
        }
    }
}

// An 8 kHz tone keeps its level at every phase (linear interpolation: 0.84
// between two frames).
static void Highs() {
    constexpr double kPi = 3.14159265358979323846;
    std::vector<float> in;
    for (int i = 0; i < 44100; ++i) {
        const float v = float(std::sin(2 * kPi * 8000 * i / 44100.0));
        in.push_back(v);
        in.push_back(-v);
    }
    std::vector<float> out;
    StreamResampler r;
    r.Push(in.data(), 44100, 1.0005, out);
    // The peak of each 2 ms window: the phase sweeps every 2000 frames.
    float lo = 2, hi = 0;
    for (size_t w = 1000; w + 88 < out.size() / 2; w += 88) {
        float peak = 0;
        for (size_t k = w; k < w + 88; ++k) peak = std::max(peak, std::fabs(out[2 * k]));
        lo = std::min(lo, peak);
        hi = std::max(hi, peak);
    }
    check(lo > 0.95f && hi < 1.01f, "the highs keep their level");
}

// The producer at 44.1 kHz of host time (a frame per call, as the SPORT
// delivers them), every 10 s stallMs late and then catching up; the card at
// 44.1 kHz * (1 + drift) in requests of request frames. The level stays near
// the target; once playing, a producer up to 8 ms late runs nothing dry and a
// later one costs one gap a stall, the ring never overflows, and the pitch is
// off the drift's by at most the gain.
static void Loop(double drift, double stallMs, const char* what, uint32_t request = 256) {
    constexpr double kRate = 44100, kBase = 768, kRing = 16384;  // MiniaudioOutput's
    const double kTarget = TargetLevel(uint32_t(kBase), request);
    PlaybackGate gate{uint32_t(kBase)};
    LevelServo servo(kBase, 0.005, 0.005, kRate / 4, kRate * 15);
    servo.SetTarget(kTarget);
    StreamResampler resampler;
    std::vector<float> out;
    double level = 0, produced = 0, card = 0;
    const double cardPeriod = request / (kRate * (1 + drift));
    double nextCard = cardPeriod;
    uint64_t underruns = 0, dropped = 0, played = 0, skipped = 0;
    double maxRatioLate = 0, minRatioLate = 2, maxRatioSettled = 0, minRatioSettled = 2;
    double lo = kRing, hi = 0, settledLo = kRing, settledHi = 0;
    const float frame[2] = {0.5f, -0.5f};
    const double seconds = 120;
    for (uint64_t n = 0; n < uint64_t(seconds * kRate); ++n) {
        double t = double(n) / kRate;
        // Every 10 s the producer is stallMs late, then catches up.
        const double inStall = std::fmod(t, 10.0);
        if (stallMs > 0 && inStall >= 5.0 && inStall < 5.0 + stallMs / 1000) t = std::floor(t / 10) * 10 + 5.0 + stallMs / 1000;
        while (nextCard <= t) {
            const bool wasPlaying = gate.Playing();
            const PlaybackGate::Frames take = gate.Take(uint32_t(level), request);
            if (wasPlaying && !gate.Playing()) ++underruns;
            played += take.play;
            skipped += take.skip;
            level -= take.skip + take.play;
            card += request;
            nextCard += cardPeriod;
            if (t > 20) { lo = std::min(lo, level); hi = std::max(hi, level); }
            if (t > 80) { settledLo = std::min(settledLo, level); settledHi = std::max(settledHi, level); }
        }
        out.clear();
        const double ratio = servo.Update(level, 1);
        // Past the start, the pitch is off the drift's rate by at most the gain.
        if (t > 20) {
            maxRatioLate = std::max(maxRatioLate, ratio);
            minRatioLate = std::min(minRatioLate, ratio);
        }
        if (t > 80) {
            maxRatioSettled = std::max(maxRatioSettled, ratio);
            minRatioSettled = std::min(minRatioSettled, ratio);
        }
        resampler.Push(frame, 1, ratio, out);
        const double frames = double(out.size() / 2);
        const double room = kRing - level;
        dropped += uint64_t(std::max(0.0, frames - room));
        level += std::min(frames, room);
        produced += frames;
    }
    std::printf("%s: level %.0f..%.0f, underruns %llu, skipped %llu, dropped %llu, ratio %.5f..%.5f\n", what, lo,
                hi, (unsigned long long)underruns, (unsigned long long)skipped, (unsigned long long)dropped,
                minRatioLate, maxRatioLate);
    const uint64_t stalls = stallMs > 0 ? uint64_t(seconds / 10) : 0;
    check(underruns <= (stallMs > 8 ? stalls : 0), "no gap once playing, or one a stall");
    check(minRatioLate > 1 - drift - 0.0055 && maxRatioLate < 1 - drift + 0.0055, "the pitch bends at most by the gain");
    if (stallMs == 0) check(maxRatioSettled - minRatioSettled < 0.001, "and holds without a stall");
    check(dropped == 0, "the ring never overflows");
    check((stallMs > 8 || lo > 0) && hi < kRing, "level between the rails");
    // Sampled after each request: the target less up to a request.
    if (stallMs == 0)
        check(settledLo > kTarget - request - 64 && settledHi < kTarget + 64, "the level settles at its target");
    if (stallMs >= 60) check(skipped > 0, "the catch-up past twice the target is skipped");
}

int main() {
    Split();
    Gate();
    Resampler();
    Highs();
    Loop(0.0, 0, "same clocks");
    Loop(0.003, 0, "card 0.3% fast");
    Loop(-0.003, 0, "card 0.3% slow");
    // Measured: a card 56 ppm fast.
    Loop(0.001, 8, "card 0.1% fast, producer 8 ms late every 10 s");
    Loop(-0.001, 8, "card 0.1% slow, producer 8 ms late every 10 s");
    Loop(0.001, 30, "card 0.1% fast, producer 30 ms late every 10 s");
    Loop(0.001, 90, "card 0.1% fast, producer 90 ms late every 10 s");
    Loop(-0.001, 90, "card 0.1% slow, producer 90 ms late every 10 s");
    Loop(0.001, 0, "card 0.1% fast, requests of 4096 frames", 4096);
    Loop(0.001, 8, "card 0.1% fast, requests of 4096 frames, producer 8 ms late", 4096);
    std::puts("Audio stream: PASS");
}
