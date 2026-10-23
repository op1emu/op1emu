// The GP timers keep TIMER_STATUS between the ticks and writes that can
// change it (firmware polls it): one instance reads it after every step, a
// second is made to rebuild it before each read, and both must agree, as must
// every other register.
#include "cpu/gptimer.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

static void Require(bool ok, const std::string& message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}

// TIMER_DISABLE of a PWM_OUT timer is a pending stop (BF52x HRM, "Stopping
// the Timer in PWM_OUT Mode"): TIMEN reads 0 at once, but the timer finishes
// its period, with its interrupt, and only then clears TRUN. Writing TRUN in
// TIMER_STATUS after the disable stops it at once, with no interrupt. The
// OP-1's PPI interrupt disables TIMER0/1 every frame and polls TRUN until
// they stop themselves.
static void DisableIsAPendingStop() {
    for (bool abort : {false, true}) {
        GPTimer timers(0xFFC00600);
        int interrupts = 0;
        timers.BindInterrupt(0, 0, [&](int, int level) { interrupts += level; });
        // TIMER0 as the OP-1 has it: PWM out, count to the period, interrupt.
        timers.Write32(0x00, 0x01 | 0x08 | 0x10);
        timers.Write32(0x08, 200);
        timers.Write32(0x0C, 199);
        timers.Write32(0x80, 1);
        for (int i = 0; i < 50; ++i) timers.Tick(GPTimerClockTypeSCLK);
        Require(timers.Read32(0x88) & (1u << 12), "an enabled timer runs (TRUN0)");
        timers.Write32(0x84, 1);
        Require(!(timers.Read32(0x80) & 1), "TIMEN0 reads 0 at the disable");
        if (abort) {
            timers.Write32(0x88, 1u << 12);
            Require(!(timers.Read32(0x88) & (1u << 12)), "TRUN0 written after the disable stops the timer at once");
        } else {
            Require(timers.Read32(0x88) & (1u << 12), "TRUN0 stays set to the end of the period");
        }
        int ticks = 50;
        while (ticks < 400 && (timers.Read32(0x88) & (1u << 12))) {
            timers.Tick(GPTimerClockTypeSCLK);
            ++ticks;
        }
        if (!abort) {
            Require(ticks == 199, "the timer stops at the end of its period, at tick " + std::to_string(ticks));
            Require(interrupts == 1, "with the period's interrupt");
        } else {
            Require(interrupts == 0, "an aborted timer raises no interrupt");
        }
        for (int i = 0; i < 400; ++i) timers.Tick(GPTimerClockTypeSCLK);
        Require(!(timers.Read32(0x88) & (1u << 12)), "a stopped timer stays stopped");
        Require(interrupts == (abort ? 0 : 1), "and raises nothing more");
    }
}

int main() {
    DisableIsAPendingStop();
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        GPTimer kept(0xFFC00600), rebuilt(0xFFC00600);
        int interrupts[2] = {};
        for (int i = 0; i < 8; ++i) {
            kept.BindInterrupt(i, 0, [&](int, int level) { interrupts[0] += level; });
            rebuilt.BindInterrupt(i, 0, [&](int, int level) { interrupts[1] += level; });
        }
        std::mt19937 rng(seed);
        auto both = [&](auto&& f) { f(kept); f(rebuilt); };
        for (int step = 0; step < 20000; ++step) {
            const uint32_t op = rng() % 16, timer = rng() % 8, value = rng();
            both([&](GPTimer& t) {
                switch (op) {
                case 0: t.Write32(timer * 0x10 + 0x00, value & 0xFF); break;           // CONFIG
                case 1: t.Write32(timer * 0x10 + 0x08, value % 40 + 1); break;         // PERIOD
                case 2: t.Write32(timer * 0x10 + 0x0C, value % 40 + 1); break;         // WIDTH
                case 3: t.Write32(0x80, 1u << timer); break;                          // ENABLE
                case 4: t.Write32(0x84, 1u << timer); break;                          // DISABLE
                case 5: t.Write32(0x88, value); break;                                // STATUS W1C
                default:
                    t.Tick(static_cast<GPTimerClockType>(value % 3));
                }
            });
            rebuilt.Write32(0x8C, 0);  // no register: only drops what it kept
            const uint32_t a = kept.Read32(0x88), b = rebuilt.Read32(0x88);
            Require(a == b, "seed " + std::to_string(seed) + " step " + std::to_string(step) + ": TIMER_STATUS " +
                                std::to_string(a) + " vs " + std::to_string(b));
            for (uint32_t offset = 0; offset < 0x88; offset += 4)
                Require(kept.Read32(offset) == rebuilt.Read32(offset), "registers differ");
            Require(interrupts[0] == interrupts[1], "interrupts differ");
        }
    }
    std::puts("gptimer: ok");
    return 0;
}
