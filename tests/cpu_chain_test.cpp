// Chaining runs the guest's work block for block as a run per block does:
// loops in L1 run until an event, once chained and once with a return after
// every block, and must end at the same packet with the same registers,
// CYCLES reads, device reads, core timer count and ILAT, and deliver every
// event a read hook queues at the same packet.
#include "cpu/coretimer.h"
#include "cpu/cpu.h"
#include "cpu/nand.h"
#include "cpu_state.h"
#include "mmr.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
    explicit TestCpu(bool chain) : BlackFinCpu(true) {
        cpuThread_.store(std::this_thread::get_id());
        AttachNandFlash(std::make_shared<NoFlash>());
        SetChaining(chain);
    }
    CoreTimer& Timer() { return *coreTimer; }
    CpuState& State() { return *cpuState_; }
};

constexpr uint32_t kCode = 0xFFA00100;
constexpr uint32_t kIsr = 0xFFA00200;     // IVG6, the core timer
constexpr uint32_t kHooked = 0x2000;      // RAM with a read hook
constexpr uint32_t kTcount = 0xFFE0300C;  // core timer TCOUNT

constexpr uint32_t kNfcIrqStat = 0xFFC03708;  // NFC_IRQSTAT
constexpr uint32_t kIpend = 0xFFE02108;       // the CEC's IPEND

enum class Loop { Plain, Hooked, Mmr, HardwareLoop, Nfc, Cec };

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

static Outcome Scenario(bool chain, Loop loop, uint64_t dueNs, bool timer) {
    TestCpu cpu(chain);
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
    unsigned runs = 0;
    while (!done && runs < 20000000) {
        cpu.Run();
        ++runs;
    }
    Require(done && !cpu.Failed(), "the loop runs until its event");
    const auto& s = cpu.State();
    return {{cpu.Time().Packets(), s.dpregs[0], s.dpregs[1], s.dpregs[2], s.dpregs[3], s.dpregs[4], hookEvents,
             cpu.Timer().Read32(0x0C), cec_mmr_read(&cpu.State(), CEC_MMR_BASE + 4 * CEC_IDX_ILAT)},
            runs};
}

int main() {
    // Isolate OTP creation performed by BlackFinCpu's constructor.
    char directory[] = "/tmp/op1-cpu-chain-test-XXXXXX";
    Require(mkdtemp(directory) != nullptr, "create isolated CPU directory");
    auto original = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    unsigned pairs = 0;
    uint64_t plainRuns = 0, chainedRuns = 0, plainLoopRuns = 0, chainedLoopRuns = 0;
    uint64_t plainQuietRuns = 0, chainedQuietRuns = 0;
    uint64_t loopIterations = 0, isrEntries = 0;
    for (Loop loop : {Loop::Plain, Loop::Hooked, Loop::Mmr, Loop::HardwareLoop, Loop::Nfc, Loop::Cec}) {
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
