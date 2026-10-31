// The emulator of gui/main.cpp for one thread, driven from JavaScript a slice
// at a time (web/README.md): bcore's WebAssembly build has no threads, so the
// CPU runs in op1_step(), between which the host (a Web Worker, or Node for
// the tests) takes keys, draws frames and plays audio. The same module runs
// in both: files are whatever the host mounted (the NAND and otp.bin in the
// working directory), and frames go to Module.onFrame.

#include "cpu/cpu.h"
#include "loader/ldr.h"
#include "peripheral/MT29F4G08.h"
#include "peripheral/keyboard.h"
#include "utils/log.h"
#include "web_audio.h"
#include "web_display.h"

#include <emscripten.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// A promotion's job to the host's tier Worker (op1_remote_tier): whether
// Module.bcoreSubmit took it.
EM_JS(int, SubmitJob, (uint32_t ticket, const uint8_t* bytes, uint32_t size), {
    try {
        const submit = Module["bcoreSubmit"];
        return submit && submit(ticket >>> 0, HEAPU8.slice(bytes >>> 0, (bytes >>> 0) + (size >>> 0))) ? 1 : 0;
    } catch (error) {
        err("op1: a tier-1 job: " + error);
        return 0;
    }
});

namespace {

// The host's clock, as BlackFinCpu::Pace() reads it (PacedUntilNs).
uint64_t WallNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// op1_step()'s own clock: performance.now(), as steady_clock's is under
// WASI's clock_time_get, without the BigInt it is stored through there
// (0.1 to 0.3 us a read in a browser's Worker; op1_step() read the clock
// after every run, 0.33 s of a boot to the main screen in Chrome 133). Not
// steady_clock's: in a browser the two count from different origins.
uint64_t StepNs() { return uint64_t(emscripten_get_now() * 1e6); }

// Where an LDR's DXE returns to (LdrExecutionThread in gui/main.cpp).
constexpr u32 kLdrReturn = 0x8000000;
// How often the sensors are fed, as gui/main.cpp's host loop does.
constexpr uint64_t kSensorNs = 16000000;
// op1_step() reads the clock after this many runs, or once the guest has
// gone this many cycles on (0.25 ms of guest time, under 1 ms of the
// host's), whichever comes first: a run took 4.6 us on average over a boot.
// A browser's clock moves by 100 us at least (5 us cross-origin isolated).
constexpr unsigned kClockRuns = 32;
constexpr uint64_t kClockCycles = 100000;
// Blocks stay at tier 0, made of bcore's stencils (copy-and-patch), unless
// the host sets a threshold (op1_tiering) for the tier Worker
// (op1_remote_tier) to compile them again: with no worker thread a
// promotion compiles on the one thread, between two blocks. Over a boot and
// the synth phase (Node, web/README.md) the default threshold stopped the
// guest for up to 225 ms at a time and took 57 s to 10 s of guest time,
// LLVM's tier 0 alone at most 19 ms and 48 s, and played the synth as fast
// within 30%.
constexpr uint32_t kTierThreshold = UINT32_MAX;
// A block's first starts make it no module of its own (bcore's cold tier,
// BlackFinCpu::SetColdTier); those that start more often are compiled 32
// to a module, or fewer after 64 Ki packets. Over a boot to the main
// screen in Node: 21,400 modules to 5,900, 1.2 GB of memory to 0.7, 9.0 s
// to 7.9; 2 to 10 starts did as well, 30 and 100 less so (most blocks that
// start often do so at once, and a cold start is slower).
constexpr uint32_t kColdThreshold = 10, kColdBatch = 32;
constexpr uint64_t kColdAge = uint64_t(1) << 16;

// op1_start's flags.
enum StartFlags : int {
    kDeterministic = 1,  // --deterministic
    kNandSnapshot = 2,   // --nand-snapshot
    kNoChain = 4,        // --no-chain
};

struct Session {
    explicit Session(bool deterministic) : cpu(deterministic) {}

    BlackFinCpu cpu;
    std::shared_ptr<WebDisplay> display = std::make_shared<WebDisplay>();
    std::shared_ptr<Keyboard> keyboard = std::make_shared<Keyboard>();
    std::shared_ptr<WebAudioOutput> audio;  // none: the SPORT drains with no sink
    LDRParser ldr;
    size_t dxe = 0;          // the LDR's DXE running, while one is
    bool ldrMode = false;
    bool finished = false;   // the LDR's last DXE returned
    // Sensors: fixed acceleration (as a headless script's `accel`) or the
    // random feed of gui/main.cpp, and the volume knob.
    bool accelFixed = false;
    int16_t ax = 0, ay = 0, az = 0;
    bool randomSent = false;
    uint64_t nextSensorNs = 0;
};

std::unique_ptr<Session> session;

void PushSensors(Session& s) {
    // Deterministic runs get one random sample, as gui/main.cpp sends.
    if (s.accelFixed || (s.cpu.Time().Deterministic() && s.randomSent)) return;
    s.randomSent = true;
    const auto ax = static_cast<int16_t>((std::rand() % (540 - 50 + 1)) + 50);
    const auto ay = static_cast<int16_t>((std::rand() % (-50 - (-540) + 1)) + (-540));
    const auto az = static_cast<int16_t>((std::rand() % (874 - 75 + 1)) + 75);
    s.cpu.SetAcceleration(ax, ay, az);
}

// The LDR's next DXE: its blocks written, and the PC at its first.
bool LoadDxe(Session& s) {
    const auto& dxes = s.ldr.getDXEs();
    if (s.dxe >= dxes.size()) return false;
    for (const auto& block : dxes[s.dxe].blocks) {
        if (!block.data.empty())
            s.cpu.GetEmulator().MemoryWrite(block.header.target_address, block.data.data(), block.data.size());
        if (block.IsFirstBlock()) {
            s.cpu.SetRegister(RegIndex::RETS, kLdrReturn);
            s.cpu.SetPC(block.header.target_address);
        }
    }
    LogInfo("Start executing DXE");
    return true;
}

// One block (gui/main.cpp's execution threads): false once the run is over.
bool RunOnce(Session& s) {
    s.cpu.Run();
    if (s.cpu.Failed()) return false;
    if (s.ldrMode && s.cpu.PC() == kLdrReturn) {
        LogInfo("Finished executing DXE");
        ++s.dxe;
        if (!LoadDxe(s)) {
            s.finished = true;
            return false;
        }
    }
    return true;
}

}  // namespace

// For web/src/gate.cpp.
BlackFinCpu* WebCpu() { return session ? &session->cpu : nullptr; }
bool WebRunOnce() { return session && RunOnce(*session); }

extern "C" {

// Starts the emulator on the NAND image at nandPath (and an LDR, unless
// ldrPath is empty), with StartFlags; rtcEpoch in seconds, or 0 for the
// clock's; audio to Module.onAudio with a ring of audioMs (web_audio.h), or
// none for 0. Returns 0, or -1 with the reason logged.
EMSCRIPTEN_KEEPALIVE int op1_start(const char* nandPath, const char* ldrPath, int flags, double rtcEpoch,
                                   int audioMs) {
    try {
        auto s = std::make_unique<Session>(flags & kDeterministic);
        BlackFinCpu& cpu = s->cpu;
        s->ldrMode = ldrPath && *ldrPath;
        if (s->ldrMode && !s->ldr.loadFile(ldrPath)) throw std::runtime_error(std::string("cannot load ") + ldrPath);
        // As gui/main.cpp, less what needs threads: no promotion worker
        // (bcore has none here), no USB/IP. BlackFinCpu turns bcore's
        // copy-and-patch tier 0 on.
        cpu.SetChaining(!(flags & kNoChain) && !s->ldrMode);
        cpu.SetTiering(kTierThreshold);
        cpu.SetColdTier(kColdThreshold, kColdBatch, kColdAge);
        if (rtcEpoch > 0)
            cpu.SetRtcEpoch(std::chrono::system_clock::time_point(std::chrono::seconds(int64_t(rtcEpoch))));
        cpu.AttachDisplay(s->display);
        cpu.AttachKeyboard(s->keyboard);
        if (audioMs > 0) {
            s->audio = std::make_shared<WebAudioOutput>(unsigned(audioMs));
            cpu.AttachAudioOutput(s->audio);
        }
        cpu.SetBootMode(0x0D);  // boot from NAND flash with port H
        cpu.AttachNandFlash(std::make_shared<MT29F4G08>(cpu, nandPath, flags & kNandSnapshot));
        cpu.SetPotentiometerValue(0xFF - 128);
        if (s->ldrMode) LoadDxe(*s);
        else cpu.SetPC(0xEF000000);  // the boot ROM
        session = std::move(s);
        return 0;
    } catch (const std::exception& e) {
        LogError("%s", e.what());
        return -1;
    }
}

// Runs for up to budgetMs of the host's time. Returns how many ms the host
// should wait before the next call (0: at once), or -1 once the run is over
// (failed, or the LDR finished; see op1_finished).
EMSCRIPTEN_KEEPALIVE double op1_step(double budgetMs) {
    if (!session) return -1;
    Session& s = *session;
    uint64_t now = StepNs();
    const uint64_t end = now + uint64_t(budgetMs * 1e6);
    double wait = 0;
    try {
        for (;;) {
            if (now >= s.nextSensorNs) {
                PushSensors(s);
                s.nextSensorNs = now + kSensorNs;
            }
            const uint64_t from = s.cpu.Time().Cycles();
            for (unsigned runs = 0; runs < kClockRuns && s.cpu.Time().Cycles() - from < kClockCycles; ++runs) {
                if (!RunOnce(s)) {
                    wait = -1;
                    break;
                }
                if (s.cpu.PacedUntilNs()) break;
            }
            if (wait < 0) break;
            if (const uint64_t until = s.cpu.PacedUntilNs()) {
                const uint64_t after = WallNs();
                wait = until > after ? double(until - after) / 1e6 : 0.0;
                break;
            }
            now = StepNs();
            if (now >= end) break;
        }
    } catch (const std::exception& e) {
        LogError("Emulation stopped: %s", e.what());
        wait = -1;
    }
    if (s.audio) s.audio->Flush();
    return wait;
}

// The AudioWorklet's report (WebAudioOutput::Report): frames it took in all,
// and whether it plays.
EMSCRIPTEN_KEEPALIVE void op1_audio_report(double consumed, int playing) {
    if (session && session->audio) session->audio->Report(consumed, playing);
}

// Tiered compilation (BlackFinCpu::SetTiering, gui/main.cpp's
// --tier-threshold and --tier-size-ref), for blocks compiled from now on.
EMSCRIPTEN_KEEPALIVE void op1_tiering(double threshold, double sizeRef) {
    if (session) session->cpu.SetTiering(uint32_t(threshold), uint32_t(sizeRef));
}

// Promotions other than loop blocks with bcore's light pipeline
// (BlackFinCpu::SetLightTier1), from now on.
EMSCRIPTEN_KEEPALIVE void op1_light_tier1(int light) {
    if (session) session->cpu.SetLightTier1(light != 0);
}

// The cold tier (BlackFinCpu::SetColdTier), for blocks compiled from now on.
EMSCRIPTEN_KEEPALIVE int op1_cold(double threshold, double batch, double age) {
    return session && session->cpu.SetColdTier(uint32_t(threshold), uint32_t(batch), uint64_t(age)) ? 0 : -1;
}

// Promotions to tier 1 compiled by the host's tier Worker
// (web/compile-worker.js, BlackFinCpu::SetRemoteTier): each job goes to
// Module.bcoreSubmit(ticket, bytes), which says whether it took it, and its
// code comes back by op1_tier_delivered, the module made of it in
// Module.bcoreModules under the ticket. on 0: compiled here again.
EMSCRIPTEN_KEEPALIVE void op1_remote_tier(int on) {
    if (!session) return;
    if (!on) {
        session->cpu.SetRemoteTier(nullptr);
        return;
    }
    session->cpu.SetRemoteTier([](uint32_t ticket, std::vector<uint8_t> job) {
        return SubmitJob(ticket, job.data(), uint32_t(job.size())) != 0;
    });
}

// The tier Worker's answer for ticket (ok 0: the job failed); whether the
// code runs now (bcore's Core::deliver).
EMSCRIPTEN_KEEPALIVE int op1_tier_delivered(double ticket, int ok, const char* name, double fallthroughPc) {
    return session && session->cpu.DeliverTier(uint32_t(ticket), ok != 0, {}, ok ? name : "", uint32_t(fallthroughPc));
}

// The tier Worker's work so far: 0 submitted, 1 refused, 2 installed, 3
// stale, 4 failed.
EMSCRIPTEN_KEEPALIVE double op1_remote_stat(int which) {
    if (!session) return 0;
    const BlackFinCpu::RemoteTierStats s = session->cpu.RemoteTierCounts();
    const uint64_t all[] = {s.submitted, s.refused, s.installed, s.stale, s.failed};
    return which >= 0 && which < 5 ? double(all[which]) : 0;
}

// Whether the LDR ran to its end (op1_step returned -1 for that).
EMSCRIPTEN_KEEPALIVE int op1_finished() { return session && session->finished; }

// A key (gui/ui.json's GPIO bank and pin) pressed or released: now, or for
// guest time delayMs after the CPU next looks (Keyboard::OnKeyAt), which a
// deterministic run queues before it starts.
EMSCRIPTEN_KEEPALIVE void op1_key(int bank, int pin, int pressed, double delayMs) {
    if (!session) return;
    if (delayMs > 0) session->keyboard->OnKeyAt(bank, pin, pressed, uint64_t(delayMs * 1e6));
    else if (pressed) session->keyboard->OnKeyPressed(bank, pin);
    else session->keyboard->OnKeyReleased(bank, pin);
}

// The volume knob, 0..255 (gui/main.cpp sends 0xFF - it).
EMSCRIPTEN_KEEPALIVE void op1_volume(int value) {
    if (session) session->cpu.SetPotentiometerValue(u8(0xFF - std::clamp(value, 0, 255)));
}

// Fixed acceleration from now on, as a headless script's `accel`.
EMSCRIPTEN_KEEPALIVE void op1_accel(int x, int y, int z) {
    if (!session) return;
    Session& s = *session;
    s.accelFixed = true;
    s.cpu.SetAcceleration(int16_t(x), int16_t(y), int16_t(z));
}

// Guest time and work so far, for progress and the tests.
EMSCRIPTEN_KEEPALIVE double op1_guest_ns() { return session ? double(session->cpu.Time().Nanoseconds()) : 0; }
EMSCRIPTEN_KEEPALIVE double op1_cycles() { return session ? double(session->cpu.Time().Cycles()) : 0; }
EMSCRIPTEN_KEEPALIVE double op1_packets() { return session ? double(session->cpu.Time().Packets()) : 0; }
EMSCRIPTEN_KEEPALIVE double op1_frames() { return session ? double(session->display->Frames()) : 0; }

}  // extern "C"
