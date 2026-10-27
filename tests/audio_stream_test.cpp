#include "audio_stream.h"
#include "realtime_pacer.h"
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
    constexpr uint32_t kForever = 1u << 30;
    PlaybackGate gate(100, kForever);
    auto take = [&](uint32_t available, uint32_t requested, uint32_t skip, uint32_t play) {
        const PlaybackGate::Frames frames = gate.Take(available, requested);
        return frames.skip == skip && frames.play == play;
    };
    check(take(99, 50, 0, 0), "waits for its level");
    check(take(100, 50, 0, 50), "starts at its level");
    check(take(30, 50, 0, 30), "plays what a short ring has");
    check(!gate.Playing(), "and waits again");
    check(take(60, 50, 0, 0), "below its level after running dry");
    check(take(150, 50, 25, 50), "starts again at its level and half a request");
    check(take(50, 50, 0, 50), "an exact request is no underrun");
    check(gate.Playing(), "still playing");
    check(take(200, 50, 0, 50), "keeps a full ring up to twice its level");
    check(take(201, 50, 76, 50), "skips down to its level and half a request above it");
    check(take(1000, 50, 875, 50), "however far above");

    PlaybackGate large(100, kForever);
    check(TargetLevel(100, 150) == 200, "a large request raises the level");
    check(TargetLevel(100, 100000) == kMaxRequestFrames + 50, "by up to kMaxRequestFrames");
    check(large.Take(199, 150).play == 0, "which the gate waits for");
    check(large.Take(200, 150).play == 150, "and then plays");
    check(large.Take(200, 150).play == 150 && large.Playing(), "a request at a time");

    // Requests past kMaxRequestFrames: the target is bounded, a skip never
    // leaves the request short.
    PlaybackGate huge(768, kForever);
    auto takeHuge = [&](uint32_t available, uint32_t requested, uint32_t skip, uint32_t play) {
        const PlaybackGate::Frames frames = huge.Take(available, requested);
        return frames.skip == skip && frames.play == play;
    };
    check(takeHuge(896, 256, 0, 256), "plays");
    check(takeHuge(10000, 10000, 0, 10000) && huge.Playing(), "a ring that holds a long request plays all of it");
    check(takeHuge(20000, 10000, 10000, 10000), "skips down to the request, past twice the target");
    check(takeHuge(10000, 16384, 0, 10000) && !huge.Playing(), "a ring short of it skips nothing");

    // The target is for the largest request of the last window or two.
    PlaybackGate window(768, 1000);
    window.Take(0, 900);
    check(window.Target() == TargetLevel(768, 900), "a long request raises the target");
    for (int i = 0; i < 4; ++i) window.Take(0, 256);
    check(window.Target() == TargetLevel(768, 900), "for this window and the next");
    for (int i = 0; i < 4; ++i) window.Take(0, 256);
    check(window.Target() == TargetLevel(768, 256), "then the shorter requests'");
}

// The pacer's rate: the servo's while the card plays what the CPU thread
// writes, else the clocks' drift it learned.
static void Fallback() {
    LevelServo servo(768, 0.005, 0.001, 0.001, 44100 / 4, 44100 * 15);
    for (int i = 0; i < 44100 * 30; ++i) servo.Update(700, 1);
    check(servo.Drift() > 1.0005 && servo.Rate() > servo.Drift(), "a low ring: faster, and a drift learned");
    constexpr uint64_t kMs = 1000000;
    check(FollowsCard(true, 1 * kMs, 2 * kMs) && PaceRate(servo, true) == servo.Rate(), "the servo's rate while playing");
    check(!FollowsCard(false, 1 * kMs, 2 * kMs) && PaceRate(servo, false) == servo.Drift(), "the drift while the gate waits");
    check(!FollowsCard(true, 200 * kMs, 2 * kMs), "and when the CPU thread stopped writing");
    check(!FollowsCard(true, 1 * kMs, 200 * kMs), "and when the card stopped asking");
}

// The CPU thread paced by a RealtimePacer at the servo's rate, making a frame
// per 1/44.1 kHz of guest time; every 10 s it stalls for stallMs and then
// runs flat out to catch up (by at most RealtimePacer::kMaxLagNs). The card
// at 44.1 kHz * (1 + drift) of host time in requests of request frames. The
// level stays near the target; once playing, a producer up to 8 ms late runs
// nothing dry and a later one costs one gap a stall, the ring never
// overflows, and guest time goes at the card's rate, off it by at most the
// servo's pull. With suspendS, the card stops asking for that long at 50 s
// (suspended): guest time goes at the drift learned meanwhile, and the card
// comes back to the target.
static void Loop(double drift, double stallMs, const char* what, uint32_t request = 256, double suspendS = 0,
                 uint32_t base = 768, double seconds = 120, uint32_t first = 0) {
    constexpr double kRate = 44100, kRing = 16384;  // MiniaudioOutput's
    const double kBase = base;
    constexpr double kGain = 0.005, kPull = 0.001, kMaxDrift = 0.001;
    constexpr uint64_t kStep = 22676;  // ns: about a frame
    const double kTarget = TargetLevel(uint32_t(kBase), request);
    PlaybackGate gate{uint32_t(kBase), uint32_t(kRate * 2)};
    LevelServo servo(kBase, kGain, kPull, kMaxDrift, kRate / 4, kRate * 15);
    RealtimePacer pacer;
    double level = 0;
    uint64_t produced = 0, lastWrite = 0, lastCallback = 0;
    const double cardPeriod = request / (kRate * (1 + drift)) * 1e9;
    double nextCard = cardPeriod;
    uint64_t underruns = 0, dropped = 0, skipped = 0, skippedLate = 0;
    uint32_t asked = first ? first : request;
    double maxRateLate = 0, minRateLate = 2, maxRateSettled = 0, minRateSettled = 2;
    double lo = kRing, hi = 0, settledLo = kRing, settledHi = 0;
    double suspendedOff = 0, learned = 0;
    for (uint64_t wall = 0; wall < uint64_t(seconds * 1e9); wall += kStep) {
        const double t = wall / 1e9;
        if (suspendS > 0 && t >= 50 && t < 50 + suspendS) nextCard = std::max(nextCard, (50 + suspendS) * 1e9);
        while (nextCard <= wall) {
            const bool wasPlaying = gate.Playing();
            const PlaybackGate::Frames take = gate.Take(uint32_t(level), asked);
            asked = request;
            if (wasPlaying && !gate.Playing()) ++underruns;
            skipped += take.skip;
            if (t > seconds / 6) skippedLate += take.skip;
            level -= take.skip + take.play;
            lastCallback = uint64_t(nextCard);
            nextCard += cardPeriod;
            if (t > seconds / 6) { lo = std::min(lo, level); hi = std::max(hi, level); }
            if (t > seconds * 2 / 3) { settledLo = std::min(settledLo, level); settledHi = std::max(settledHi, level); }
        }
        // Every 10 s the CPU thread is stallMs late, then catches up.
        const double inStall = std::fmod(t, 10.0);
        if (stallMs > 0 && inStall >= 5.0 && inStall < 5.0 + stallMs / 1000) continue;
        pacer.SetRate(PaceRate(servo, FollowsCard(gate.Playing(), wall - lastWrite, wall - lastCallback)), wall);
        pacer.Sync(produced * 1000000000 / 44100, wall);
        const uint64_t due = pacer.GuestNsAt(wall) * 441 / 10000000;
        for (; produced < due; ++produced) {
            const double room = kRing - level;
            if (room < 1) ++dropped; else level += 1;
            if (FollowsCard(gate.Playing(), 0, wall - lastCallback)) {
                servo.SetTarget(gate.Target());
                servo.Update(level, 1);
            }
            lastWrite = wall;
        }
        const double rate = 1.0 + pacer.RatePpm() / 1e6;
        if (suspendS > 0 && t < 50) learned = servo.Drift();
        if (suspendS > 0 && t >= 50.2 && t < 50 + suspendS)
            suspendedOff = std::max(suspendedOff, std::fabs(rate - learned));
        if (suspendS > 0 && t >= 50 && t < 52 + suspendS) continue;
        if (t > seconds / 6) { maxRateLate = std::max(maxRateLate, rate); minRateLate = std::min(minRateLate, rate); }
        if (t > seconds * 2 / 3) { maxRateSettled = std::max(maxRateSettled, rate); minRateSettled = std::min(minRateSettled, rate); }
    }
    std::printf("%s: level %.0f..%.0f, underruns %llu, skipped %llu, dropped %llu, rate %.5f..%.5f\n", what, lo, hi,
                (unsigned long long)underruns, (unsigned long long)skipped, (unsigned long long)dropped, minRateLate,
                maxRateLate);
    const uint64_t stalls = stallMs > 0 ? uint64_t(seconds / 10) : 0;
    if (suspendS > 0) {
        check(suspendedOff < 0.00001 && std::fabs(learned - (1 + drift)) < 0.0002,
              "a suspended card: guest time at the drift learned");
        check(underruns == 0 && settledLo > kTarget - request - 64 && settledHi < kTarget + 64,
              "and the card back at the target");
        return;
    }
    check(dropped == 0, "the ring never overflows");
    if (std::fabs(drift) > kMaxDrift + kPull) {
        // Past the servo: the gate keeps the level, by gaps or skips.
        check(drift > 0 ? underruns > 0 : skipped > 0, "a card off by more than the servo takes is the gate's");
        check(minRateLate >= 1 - kMaxDrift - kPull - 1e-6 && maxRateLate <= 1 + kMaxDrift + kPull + 1e-6,
              "and the rate stays within the servo's");
        return;
    }
    check(underruns <= (stallMs > 8 ? stalls : 0), "no gap once playing, or one a stall");
    check(minRateLate > 1 + drift - kPull - 0.0002 && maxRateLate < 1 + drift + kPull + 0.0002,
          "guest time goes at the card's rate, off it by at most the pull");
    // The level's steps ripple its average: by more for longer requests.
    if (stallMs == 0) check(skippedLate == 0, "nothing skipped without a stall");
    if (stallMs == 0)
        check(maxRateSettled - minRateSettled < (request > 1024 ? 0.0006 : 0.0002), "and holds there without a stall");
    check((stallMs > 8 || lo > 0) && hi < kRing, "level between the rails");
    // Sampled after each request: the target less up to a request.
    if (stallMs == 0)
        check(settledLo > kTarget - request - kTarget / 12 && settledHi < kTarget + kTarget / 12,
              "the level settles at its target");
    if (stallMs >= 60) check(skipped > 0, "the catch-up past twice the target is skipped");
}

int main() {
    Split();
    Gate();
    Fallback();
    Loop(0.0, 0, "same clocks");
    // Measured: a card 56 ppm fast.
    Loop(0.0005, 0, "card 0.05% fast");
    Loop(-0.0005, 0, "card 0.05% slow");
    Loop(0.003, 0, "card 0.3% fast, past the servo");
    Loop(-0.003, 0, "card 0.3% slow, past the servo");
    Loop(0.0005, 8, "card 0.05% fast, producer 8 ms late every 10 s");
    Loop(-0.0005, 8, "card 0.05% slow, producer 8 ms late every 10 s");
    Loop(0.0005, 30, "card 0.05% fast, producer 30 ms late every 10 s");
    Loop(0.0005, 90, "card 0.05% fast, producer 90 ms late every 10 s");
    Loop(-0.0005, 90, "card 0.05% slow, producer 90 ms late every 10 s");
    Loop(0.0005, 250, "card 0.05% fast, producer 250 ms late every 10 s");
    Loop(0.0005, 0, "card 0.05% fast, suspended for 3 s", 256, 3);
    Loop(0.0005, 0, "card 0.05% fast, requests of 4096 frames", 4096);
    Loop(0.0, 0, "same clocks, a first request of 2048 frames", 256, 0, 768, 300, 2048);
    // The longest ring (--audio-buffer 200): 133 ms.
    Loop(0.0005, 0, "card 0.05% fast, a 133 ms ring", 256, 0, 5865, 600);
    Loop(-0.0005, 30, "card 0.05% slow, a 133 ms ring, producer 30 ms late", 256, 0, 5865, 600);
    Loop(0.0005, 8, "card 0.05% fast, requests of 4096 frames, producer 8 ms late", 4096);
    std::puts("Audio stream: PASS");
}
