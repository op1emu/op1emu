// Chaining runs the guest's work block for block as a run per block does:
// loops in L1 run until an event, once chained and once with a return after
// every block, and must end at the same packet with the same registers,
// CYCLES reads, device reads, core timer count and ILAT, and deliver every
// event a read hook queues at the same packet.
#include "cpu/coretimer.h"
#include "cpu/gptimer.h"
#include "cpu/cpu.h"
#include "cpu/nand.h"
#include "cpu_state.h"
#include "mmr.h"
#include "../ext/bcore/src/cec.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct NoFlash : NandFlash {
    void SendCommand(u8) override {}
    void SendAddress(u8) override {}
    u8 ReadData() override { return 0xff; }
    void WriteData(u8) override {}
    bool IsDataReady() const override { return true; }
    bool IsBusy() const override { return false; }
    void StartPageRead() override {}
    void StartPageWrite() override {}
    u32 PageRead(u8*, u32) override { return 0; }
    u32 PageWrite(const u8*, u32) override { return 0; }
    void SetReadCallback(ReadCallback) override {}
};

class TestCpu : public BlackFinCpu {
public:
    explicit TestCpu(bool chain, bool deterministic = true) : BlackFinCpu(deterministic) {
        cpuThread_.store(std::this_thread::get_id());
        AttachNandFlash(std::make_shared<NoFlash>());
        SetChaining(chain);
    }
    CoreTimer& Timer() { return *coreTimer; }
    CpuState& State() { return *cpuState_; }
    GPTimer& GpTimer() { return *gptimer; }
    // Guest time goes on by packets with no host work, or with it after,
    // between runs: CpuState's CEC flag bound as Core::run binds it.
    void Retire(uint64_t packets) { time_.Retire(packets); }
    void HostWorkNow() {
        cpuState_->cec_pending = &bcore_cec_pending;
        HostWork(false);
    }
};

constexpr uint32_t kCode = 0xFFA00100;
constexpr uint32_t kIsr = 0xFFA00200;     // IVG6, the core timer
constexpr uint32_t kHooked = 0x2000;      // RAM with a read hook
constexpr uint32_t kTcount = 0xFFE0300C;  // core timer TCOUNT

constexpr uint32_t kNfcIrqStat = 0xFFC03708;  // NFC_IRQSTAT
constexpr uint32_t kIpend = 0xFFE02108;       // the CEC's IPEND

enum class Loop { Plain, Hooked, Mmr, HardwareLoop, Nfc, Cec, Idle };

// Leaves reset for IVG15, which runs the loop in supervisor mode with
// interrupts on (bcore latches no line in user mode):
// RAISE 15; R7.L = 0x010e; R7.H = 0xFFA0; RETI = R7; RTI; JUMP.S 0;
static const std::vector<uint16_t> kPrologue = {0x009f, 0xe107, 0x010e, 0xe147, 0xffa0, 0x3e1f, 0x0011, 0x2000};
constexpr uint32_t kIvg15 = kCode + 0x10;
// SP.L = 0x1000; SP.H = 0xFFB0; [--SP] = RETI; (top of L1 scratch)
static const std::vector<uint16_t> kIvg15Entry = {0xe10e, 0x1000, 0xe14e, 0xffb0, 0x017b};
// R3 += 1; R4 = CYCLES; RTI;
static const std::vector<uint16_t> kIsrCode = {0x640b, 0x31a6, 0x0011};

static std::vector<uint16_t> Code(Loop loop) {
    switch (loop) {
    case Loop::Plain:  // L: R0 += 1; R2 = CYCLES; JUMP.S L;
        return {0x6408, 0x3196, 0x2ffe};
    case Loop::Hooked:
    case Loop::Mmr:
    case Loop::Nfc:
    case Loop::Cec:  // L: R0 += 1; R1 = [P0]; R2 = CYCLES; JUMP.S L;
        return {0x6408, 0x9101, 0x3196, 0x2ffd};
    case Loop::HardwareLoop:  // L: P1 = 100; LSETUP (B, B) LC0 = P1; B: R0 += 1; R2 = CYCLES; JUMP.S L;
        return {0xe129, 0x0064, 0xe0a2, 0x1002, 0x6408, 0x3196, 0x2ffa};
    case Loop::Idle:  // L: R0 += 1; R2 = CYCLES; IDLE; JUMP.S L;
        return {0x6408, 0x3196, 0x0020, 0x2ffd};
    }
    return {};
}

static void Write(Emulator& emu, uint32_t at, const std::vector<uint16_t>& code) {
    for (uint16_t op : code) {
        emu.MemoryWrite16(at, op);
        at += 2;
    }
}

// What the guest and the devices did, compared between the two runs.
struct Outcome {
    static constexpr const char* kNames[] = {"packets", "R0", "R1", "R2", "R3", "R4", "hook events", "TCOUNT", "ILAT"};
    std::array<uint64_t, std::size(kNames)> state;
    unsigned runs;
};

static Outcome Scenario(bool chain, Loop loop, uint64_t dueNs, bool timer, bool deterministic = true,
                        const std::function<void(TestCpu&)>& started = {}) {
    TestCpu cpu(chain, deterministic);
    auto& emu = cpu.GetEmulator();
    Write(emu, kCode, kPrologue);
    Write(emu, kIvg15, kIvg15Entry);
    Write(emu, kIvg15 + 2 * kIvg15Entry.size(), Code(loop));
    evt_write(EVT_BASE + 4 * 15, kIvg15);
    const uint32_t imask = CEC_MMR_BASE + 4 * CEC_IDX_IMASK;
    const uint32_t ivgs = 1u << 15 | (timer ? 1u << 6 : 0);
    cec_mmr_write(&cpu.State(), imask, cec_mmr_read(&cpu.State(), imask) | ivgs);
    // The packets at which the hook's events ran, hashed.
    uint64_t hookEvents = 0;
    if (loop == Loop::Hooked) {
        cpu.State().dpregs[8] = kHooked;
        emu.AddReadHook(kHooked, [&](u32) {
            cpu.QueueEvent([&] { hookEvents = hookEvents * 31 + cpu.Time().Packets() + 1; },
                           std::chrono::nanoseconds(1));
            return 0x1234;
        });
    }
    if (loop == Loop::Mmr) cpu.State().dpregs[8] = kTcount;
    if (loop == Loop::Nfc) cpu.State().dpregs[8] = kNfcIrqStat;
    if (loop == Loop::Cec) cpu.State().dpregs[8] = kIpend;
    // No system interrupt wakes the idle loop (the NFC's is asserted here):
    // only the host's due times and the core timer do.
    if (loop == Loop::Idle) {
        emu.MemoryWrite32(0xFFC00124, 0);  // SIC_IWR0
        emu.MemoryWrite32(0xFFC00164, 0);  // SIC_IWR1
    }
    if (timer) {
        // Every expiry enters the ISR, at the packet the host latches it.
        Write(emu, kIsr, kIsrCode);
        evt_write(EVT_BASE + 4 * 6, kIsr);
        cpu.Timer().Write32(0x04, 11);  // TPERIOD
        cpu.Timer().Write32(0x00, 7);   // TMPWR | TMREN | TAUTORLD
    }
    bool done = false;
    cpu.QueueEvent([&] { done = true; }, std::chrono::nanoseconds(dueNs));
    cpu.SetPC(kCode);
    if (started) started(cpu);
    unsigned runs = 0;
    while (!done && runs < 20000000) {
        cpu.Run();
        ++runs;
    }
    Require(done && !cpu.Failed(), "the loop runs until its event");
    // An idle loop waits for its wakeups (BlackFinCpu::Wait), here the GP
    // timers' tick every 10000 cycles: CYCLES keeps counting, the core does
    // not, and the loop reads CYCLES after each wakeup.
    if (loop == Loop::Idle && !timer && dueNs > 50000)
        Require(cpu.Time().Packets() * 100 < cpu.Time().Cycles() && cpu.State().dpregs[2] > 30000,
                "an IDLE lets guest time pass without packets, CYCLES included");
    const auto& s = cpu.State();
    return {{cpu.Time().Packets(), s.dpregs[0], s.dpregs[1], s.dpregs[2], s.dpregs[3], s.dpregs[4], hookEvents,
             cpu.Timer().Read32(0x0C), cec_mmr_read(&cpu.State(), CEC_MMR_BASE + 4 * CEC_IDX_ILAT)},
            runs};
}

// An IDLE whose wakeup the host's work after the block delivers -- an
// interrupt raised straight at the CEC, which no SIC wakeup shows -- does not
// wait any more: the ISR runs at the cycle it was raised, not at the next due
// time (BlackFinCpu::Wait).
static void IdleTakesItsInterrupt() {
    TestCpu cpu(true);
    auto& emu = cpu.GetEmulator();
    Write(emu, kCode, kPrologue);
    constexpr uint32_t kIdle = 0xFFA00300;
    auto entry = kIvg15Entry;  // then JUMP.S kIdle
    entry.push_back(uint16_t(0x2000 | (((kIdle - (kIvg15 + 2 * entry.size())) / 2) & 0xfff)));
    Write(emu, kIvg15, entry);
    Write(emu, kIdle, {0x0020, 0x2000});  // IDLE; JUMP.S 0
    Write(emu, kIsr, {0x640b, 0x2000});   // R3 += 1; JUMP.S 0
    evt_write(EVT_BASE + 4 * 15, kIvg15);
    evt_write(EVT_BASE + 4 * 7, kIsr);
    cec_mmr_write(&cpu.State(), CEC_MMR_BASE + 4 * CEC_IDX_IMASK, 0xffff);
    emu.MemoryWrite32(0xFFC00124, 0);  // SIC_IWR0
    emu.MemoryWrite32(0xFFC00164, 0);  // SIC_IWR1
    cpu.SetPC(kCode);
    for (unsigned i = 0; cpu.State().pc != kIdle && i < 10; ++i) cpu.Run();
    Require(cpu.State().pc == kIdle, "the guest reaches its IDLE");
    uint64_t raised = 0;
    cpu.QueueEvent([&] { raised = cpu.Time().Cycles(); cec_raise(&cpu.State(), 7); }, std::chrono::nanoseconds(1));
    cpu.Run();
    Require(raised && cpu.State().pc == kIsr, "the host's event raises IVG7, and its ISR is next");
    Require(cpu.Time().Cycles() == raised, "an IDLE woken by the interrupt taken after its block waits no more");
}

// Real-time mode (BlackFinCpu(false)) runs the guest as deterministic mode
// does, but no faster than the host's clock: an idle loop's 30 ms of guest
// time take 30 ms. A key from another thread meanwhile reaches the guest at
// about the guest time of its wall time, not after a wait it was queued in
// (it ends that wait early, as a wakeup without an interrupt would).
static void RealTimeIdleKeepsToTheHostClock() {
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    constexpr uint64_t kDueNs = 30000000, kKeyNs = 10000000;
    // The RTC's next second is a due time, which ends an IDLE's wait: both
    // modes start its calendar alike, real-time mode's own epoch being the
    // host's. On a whole second the 30 ms cross none; just over 10 ms before
    // one they cross it off the GP timers' ticks, and the loop goes round
    // once more.
    const std::chrono::system_clock::time_point second{std::chrono::seconds(TimeSource::kDefaultEpochSeconds)};
    uint64_t rounds[2] = {};
    Clock::time_point start;
    for (const auto epoch : {second, second + 1s - 10ms - 3us}) {
        const auto at = [epoch](TestCpu& cpu) { cpu.SetRtcEpoch(epoch); };
        const auto deterministic = Scenario(true, Loop::Idle, kDueNs, false, true, at);
        const auto paced = Scenario(true, Loop::Idle, kDueNs, false, false, [&](TestCpu& cpu) {
            at(cpu);
            start = Clock::now();
        });
        const auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        Require(took >= int64_t(kDueNs) - 2000000, "30 ms of guest time take about 30 ms on the host's clock");
        Require(paced.state == deterministic.state, "real-time mode executes what deterministic mode does");
        rounds[epoch != second] = deterministic.state[1];
    }
    Require(rounds[1] == rounds[0] + 1, "the RTC's next second wakes the idle loop once more");

    std::thread key;
    std::atomic<int64_t> keyedNs{-1};
    Scenario(true, Loop::Idle, kDueNs, false, false, [&](TestCpu& cpu) {
        start = Clock::now();
        key = std::thread([&] {
            std::this_thread::sleep_until(start + std::chrono::nanoseconds(kKeyNs));
            cpu.QueueEvent([&] { keyedNs = int64_t(cpu.Time().Nanoseconds()); });
        });
        // A late key thread holds the CPU up here rather than outliving it.
        cpu.QueueEvent([&] { key.join(); }, std::chrono::nanoseconds(kDueNs - 1000000));
    });
    Require(keyedNs >= 0 && keyedNs < int64_t(kKeyNs) + 6000000, "the key arrives within a few ms of its wall time");
}

// TMRCLK (TIMER7's output, ~5.7 MHz) is counted at the host's work, many
// ticks at once: what the timers on it count must not depend on when that
// work comes. Each scenario does the same register writes at the same guest
// cycles, with host work every `step` cycles, and the timers must end alike.
// A change of TIMER7's clock (a buffered period, a pending stop) comes at an
// SCLK tick and a register write at its cycle, between the host's works.
struct TmrclkWrite {
    uint64_t cycle;
    uint32_t offset, value;
};

static std::array<uint32_t, 5> TmrclkRun(uint64_t step, const std::vector<TmrclkWrite>& writes, uint64_t cycles) {
    TestCpu cpu(true);
    GPTimer& t = cpu.GpTimer();
    // TIMER7: continuous PWM on SCLK, period 14 (TMRCLK = SCLK/14).
    t.Write32(0x70, 0x09);
    t.Write32(0x78, 14);
    t.Write32(0x7C, 3);
    // TIMER0: continuous PWM on TMRCLK, a long period.
    t.Write32(0x00, 0xA9);
    t.Write32(0x08, 1000000);
    t.Write32(0x0C, 199);
    // TIMER1: PWM on TMRCLK with a short period, enabled later.
    t.Write32(0x10, 0xB9);
    t.Write32(0x18, 300);
    t.Write32(0x1C, 1);
    t.Write32(0x80, 0x81);
    uint64_t now = 0, nextWork = step;
    size_t w = 0;
    while (now < cycles) {
        const uint64_t next = std::min({nextWork, cycles, w < writes.size() ? writes[w].cycle : UINT64_MAX});
        cpu.Retire(next - now);
        now = next;
        for (; w < writes.size() && writes[w].cycle == now; ++w) t.Write32(writes[w].offset, writes[w].value);
        if (now == nextWork) {
            cpu.HostWorkNow();
            nextWork += step;
        }
    }
    cpu.HostWorkNow();
    return {t.Read32(0x04), t.Read32(0x14), t.Read32(0x74), t.Read32(0x88), t.Read32(0x78)};
}

static void TmrclkCountsAlikeAtAnyHostWork() {
    const std::vector<std::pair<const char*, std::vector<TmrclkWrite>>> scenarios = {
        {"steady", {}},
        {"TIMER7's period buffered to 28", {{0, 0x78, 28}}},
        {"TIMER7 disabled, stopping at the end of its period", {{0, 0x84, 0x80}}},
        {"TIMER1 enabled between host works", {{65003, 0x80, 0x02}}},
        {"TIMER1 enabled, then TIMER7 aborted", {{65003, 0x80, 0x02}, {97001, 0x84, 0x80}, {97001, 0x88, 1u << 31}}},
    };
    for (const auto& [name, writes] : scenarios) {
        const auto fine = TmrclkRun(10, writes, 260000);
        for (uint64_t step : {7u, 3333u, 10000u, 65536u}) {
            const auto coarse = TmrclkRun(step, writes, 260000);
            if (coarse != fine) {
                std::fprintf(stderr, "%s, host work every %llu cycles:", name, static_cast<unsigned long long>(step));
                for (size_t i = 0; i < fine.size(); ++i) std::fprintf(stderr, " %u/%u", coarse[i], fine[i]);
                std::fprintf(stderr, "\n");
                Require(false, "TMRCLK timers count alike whenever the host works");
            }
        }
        if (writes.empty() && fine[0] != 260000 / 70) std::fprintf(stderr, "TIMER0 at %u\n", fine[0]);
        if (writes.empty()) Require(fine[0] == 260000 / 70, "TIMER0 counts TMRCLK, a tick per 70 cycles");
    }
}

int main() {
    // Isolate OTP creation performed by BlackFinCpu's constructor.
    char directory[] = "/tmp/op1-cpu-chain-test-XXXXXX";
    Require(mkdtemp(directory) != nullptr, "create isolated CPU directory");
    auto original = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    IdleTakesItsInterrupt();
    RealTimeIdleKeepsToTheHostClock();
    TmrclkCountsAlikeAtAnyHostWork();
    unsigned pairs = 0;
    uint64_t plainRuns = 0, chainedRuns = 0, plainLoopRuns = 0, chainedLoopRuns = 0;
    uint64_t plainQuietRuns = 0, chainedQuietRuns = 0;
    uint64_t loopIterations = 0, isrEntries = 0;
    for (Loop loop : {Loop::Plain, Loop::Hooked, Loop::Mmr, Loop::HardwareLoop, Loop::Nfc, Loop::Cec, Loop::Idle}) {
        for (uint64_t dueNs : {1u, 2u, 3u, 5u, 7u, 999u, 1000u, 1001u, 25001u, 100001u}) {
            for (bool timer : {false, true}) {
                const Outcome plain = Scenario(false, loop, dueNs, timer);
                const Outcome chained = Scenario(true, loop, dueNs, timer);
                if (plain.state != chained.state) {
                    std::fprintf(stderr, "loop %d, event at %llu ns, timer %d:\n", int(loop),
                                 static_cast<unsigned long long>(dueNs), timer);
                    for (const Outcome* o : {&plain, &chained}) {
                        std::fprintf(stderr, " ");
                        for (size_t i = 0; i < o->state.size(); ++i)
                            std::fprintf(stderr, " %s=%#llx", Outcome::kNames[i],
                                         static_cast<unsigned long long>(o->state[i]));
                        std::fprintf(stderr, " runs=%u\n", o->runs);
                    }
                    Require(false, "chained and unchained runs do the same work");
                }
                ++pairs;
                loopIterations += chained.state[1];
                isrEntries += chained.state[4];
                plainRuns += plain.runs;
                chainedRuns += chained.runs;
                if (!timer && (loop == Loop::Plain || loop == Loop::HardwareLoop)) {
                    plainLoopRuns += plain.runs;
                    chainedLoopRuns += chained.runs;
                }
                if (!timer && (loop == Loop::Nfc || loop == Loop::Cec) && dueNs > 1000) {
                    plainQuietRuns += plain.runs;
                    chainedQuietRuns += chained.runs;
                }
            }
        }
    }
    Require(loopIterations && isrEntries, "the loops ran in IVG15 and the core timer interrupted them");
    // Loops that touch no device and are not interrupted return only at a
    // due time.
    Require(chainedLoopRuns * 10 < plainLoopRuns, "loops without device accesses chain");
    // So do loops whose device accesses leave the host nothing to do.
    Require(chainedQuietRuns * 10 < plainQuietRuns, "loops polling a quiet device register chain");
    std::filesystem::current_path(original);
    std::filesystem::remove_all(directory);
    std::printf("cpu-chain-test: ok, %u pairs, runs %llu unchained, %llu chained\n", pairs,
                static_cast<unsigned long long>(plainRuns), static_cast<unsigned long long>(chainedRuns));
}
