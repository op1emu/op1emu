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

namespace {

uint64_t WallNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// Where an LDR's DXE returns to (LdrExecutionThread in gui/main.cpp).
constexpr u32 kLdrReturn = 0x8000000;
// How often the sensors are fed, as gui/main.cpp's host loop does.
constexpr uint64_t kSensorNs = 16000000;
// Blocks stay at tier 0, made of bcore's stencils (copy-and-patch). With no
// worker thread a promotion compiles on the one thread, between two blocks:
// over a boot and the synth phase (Node, web/README.md) the default
// threshold stopped the guest for up to 225 ms at a time and took 57 s to
// 10 s of guest time, LLVM's tier 0 alone at most 19 ms and 48 s, and
// played the synth as fast within 30%.
constexpr uint32_t kTierThreshold = UINT32_MAX;

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
    const uint64_t start = WallNs(), end = start + uint64_t(budgetMs * 1e6);
    double wait = 0;
    try {
        for (uint64_t now = start; now < end; now = WallNs()) {
            if (now >= s.nextSensorNs) {
                PushSensors(s);
                s.nextSensorNs = now + kSensorNs;
            }
            if (!RunOnce(s)) {
                wait = -1;
                break;
            }
            if (const uint64_t until = s.cpu.PacedUntilNs()) {
                const uint64_t after = WallNs();
                wait = until > after ? double(until - after) / 1e6 : 0.0;
                break;
            }
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
