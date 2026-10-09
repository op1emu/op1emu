#include "loader/ldr.h"
#include "cpu/cpu.h"
#include "peripheral/MT29F4G08.h"
#include "utils/log.h"
#include "glfw_display.h"
#include "headless.h"
#include "audio_output_miniaudio.h"
#include "usbipd.h"
#include <vector>
#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <string>
#include <optional>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

std::atomic<bool> cpuShouldStop(false);

// Headless runs are stopped with SIGTERM/SIGINT. Default termination bypasses
// destructors and loses buffered output, so request a clean shutdown instead.
// The handler only stores to a lock-free atomic on the supported host.
static_assert(std::atomic<bool>::is_always_lock_free);
static void StopSigHandler(int) {
    cpuShouldStop.store(true);
}

void LdrExecutionThread(BlackFinCpu& cpu, const LDRParser& parser) {
    const auto& dxes = parser.getDXEs();
    for (const auto& dxe : dxes) {
        for (const auto& block : dxe.blocks) {
            if (!block.data.empty()) {
                cpu.GetEmulator().MemoryWrite(
                    block.header.target_address,
                    block.data.data(),
                    block.data.size()
                );
            }
            if (block.IsFirstBlock()) {
                cpu.SetRegister(RegIndex::RETS, 0x8000000);
                cpu.SetPC(block.header.target_address);
            }
        }
        LogInfo("Start executing DXE");

        while (!cpuShouldStop.load()) {
            cpu.Run();
            if (cpu.Failed()) cpuShouldStop.store(true);

            if (cpu.PC() == 0x8000000) {
                LogInfo("Finished executing DXE");
                break;
            }
            if (cpu.PC() == 0xffa06e54) {
                LogInfo("Hit delay(%d)", cpu.GetRegister(RegIndex::R0));
            }
            if (cpu.PC() == 0xffa06eec) {
                LogInfo("Hit delay end");
            }
        }

        if (cpuShouldStop.load()) {
            break;
        }
    }
    cpuShouldStop.store(true);
    LogInfo("CPU thread exiting");
}

void BootExcutionThread(BlackFinCpu& cpu) {
    cpu.SetPC(0xEF000000); // Boot entry point
    while (!cpuShouldStop.load()) {
        cpu.Run();
        if (cpu.Failed()) cpuShouldStop.store(true);
    }
    cpuShouldStop.store(true);
    LogInfo("CPU thread exiting");
}

static int RunEmulator(int argc, char* argv[]) {
    // Separate flags from positional args so flag order relative to
    // <nand_flash_file> [ldr_file] doesn't matter.
    std::vector<std::string> positional;
    // Persist guest writes by default, matching real hardware. --nand-snapshot
    // opens the image copy-on-write (backing file untouched) for disposable
    // runs or deterministic tests; --nand-rw is accepted as an explicit
    // persistent-mode selector.
    bool nandSnapshot = false;
    bool headless = false;
    bool deterministic = false;
    std::optional<long long> rtcEpoch;
    bool help = false;
    bool options = true;
    std::string inputScript;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (options && arg == "--") options = false;
        else if (options && arg == "--nand-rw") nandSnapshot = false;
        else if (options && arg == "--nand-snapshot") nandSnapshot = true;
        else if (options && arg == "--headless") headless = true;
        else if (options && arg == "--deterministic") deterministic = true;
        else if (options && arg == "--rtc-epoch") {
            if (++i == argc) throw std::runtime_error("--rtc-epoch requires seconds since 1970");
            size_t used = 0;
            long long seconds = -1;
            try { seconds = std::stoll(argv[i], &used); } catch (const std::exception&) {}
            if (used == 0 || argv[i][used] != '\0' || seconds < 0)
                throw std::runtime_error(std::string("Invalid --rtc-epoch: ") + argv[i]);
            if (seconds > TimeSource::kMaxEpochSeconds)
                throw std::runtime_error(std::string("--rtc-epoch past the RTC's range (2059-09-18, ") +
                                         std::to_string(TimeSource::kMaxEpochSeconds) + "): " + argv[i]);
            rtcEpoch = seconds;
        }
        else if (options && (arg == "--help" || arg == "-h")) help = true;
        else if (options && arg == "--input-script") {
            if (++i == argc) throw std::runtime_error("--input-script requires a path");
            inputScript = argv[i];
            if (inputScript.empty()) throw std::runtime_error("--input-script requires a nonempty path");
        } else if (options && arg.rfind("-", 0) == 0) {
            throw std::runtime_error("Unknown option: " + arg);
        } else positional.push_back(arg);
    }

    if (help || positional.empty()) {
        std::cout << "Usage: " << argv[0]
                  << " <nand_flash_file> [ldr_file] [--nand-rw|--nand-snapshot]\n"
                     "  --headless            No GLFW window or host audio device\n"
                     "  --input-script PATH   Read headless commands from PATH instead of stdin\n"
                     "  --deterministic       Guest time from executed instructions, not host time:\n"
                     "                        repeatable runs, as fast as the host allows\n"
                     "  --rtc-epoch SECONDS   RTC time at boot with --deterministic (default 2024-01-01)\n"
                     "Headless commands (one per line):\n"
                     "  keys | press KEY | release KEY | tap KEY [MS] | wait MS | quit\n"
                     "  accel X Y Z | accel random | volume 0..255\n"
                     "  wait-frames N [TIMEOUT_MS] | screenshot PATH.ppm\n"
                     "Keys are button names from gui/ui.json. Tap defaults to 100ms.\n"
                     "Wait/tap use wall time. EOF leaves the emulator running; quit or Ctrl-C stops it.\n";
        return help ? 0 : 1;
    }
    if (positional.size() > 2) throw std::runtime_error("Expected NAND and optional LDR paths");
    if (!inputScript.empty() && !headless)
        throw std::runtime_error("--input-script requires --headless");
    if (rtcEpoch && !deterministic)
        throw std::runtime_error("--rtc-epoch requires --deterministic");

    std::shared_ptr<GLFWDisplay> window;
    std::shared_ptr<HeadlessFrontend> console;
    std::shared_ptr<Display> display;
    std::shared_ptr<Keyboard> keyboard;
    if (headless) {
        int fd = inputScript.empty() ? STDIN_FILENO : open(inputScript.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) throw std::runtime_error("Failed to open input script: " + inputScript);
        try {
            console = std::make_shared<HeadlessFrontend>("gui/ui.json", fd);
        } catch (...) {
            if (!inputScript.empty()) close(fd);
            throw;
        }
        if (!inputScript.empty()) close(fd);
        display = console;
        keyboard = console;
        LogInfo("Headless mode: commands from %s", inputScript.empty() ? "stdin" : inputScript.c_str());
    } else {
        window = std::make_shared<GLFWDisplay>();
        display = window;
        keyboard = window;
    }

    std::signal(SIGTERM, StopSigHandler);
    std::signal(SIGINT, StopSigHandler);

    // Create BlackFin CPU
    BlackFinCpu cpu(deterministic);
    if (rtcEpoch) cpu.SetRtcEpoch(std::chrono::system_clock::time_point(std::chrono::seconds(*rtcEpoch)));
    cpu.AttachDisplay(display);
    cpu.AttachKeyboard(keyboard);
    // SPORT still drains DMA and advances its sample accounting without a
    // host sink. Avoid opening ALSA/PulseAudio devices on headless hosts.
    if (!headless) cpu.AttachAudioOutput(std::make_shared<MiniaudioOutput>());
    cpu.SetBootMode(0x0D); // Set BMODE to 0b1101, boot from NAND flash with port H

    auto loop = uvw::loop::get_default();
    std::thread uvloop([loop]() {
        while (true) {
            loop->run();
        }
    });
    // Detached: this thread intentionally runs until process exit. A joinable
    // std::thread destroyed at return would call std::terminate, masking the
    // exit code of a clean quit/SIGTERM shutdown.
    uvloop.detach();
    USBIPServer usbipd(*loop, cpu.GetUSB());
    usbipd.Start();

    // Load LDR file
    LDRParser parser;
    if (positional.size() > 1 && !parser.loadFile(positional[1])) {
        std::cerr << "Failed to load LDR file: " << positional[1] << std::endl;
        return 1;
    }

    // Load NAND Flash underlying storage
    auto nandFlash = std::make_shared<MT29F4G08>(cpu, positional[0], nandSnapshot);
    if (nandSnapshot)
        LogInfo("NAND flash image opened read-only (snapshot mode; guest writes will be discarded)");
    cpu.AttachNandFlash(nandFlash);

    uint64_t sensorVersion = UINT64_MAX; // forces the first headless push
    auto pushSensors = [&]() {
        const bool fixedAccel = headless && console->GetSensors().accelFixed;
        const bool sensorsChanged = headless && console->SensorVersion() != sensorVersion;
        if (!fixedAccel) {
            int16_t ax = static_cast<int16_t>((std::rand() % (540 - 50 + 1)) + 50); // ax in [50, 540]
            int16_t ay = static_cast<int16_t>((std::rand() % (-50 - (-540) + 1)) + (-540)); // ay in [-540, -50]
            int16_t az = static_cast<int16_t>((std::rand() % (874 - 75 + 1)) + 75); // az in [75, 874]
            cpu.SetAcceleration(ax, ay, az); // Placeholder for random accelerometer data
        }
        if (headless) {
            // Script-set sensors are pushed once per change; the CPU keeps the value.
            if (sensorsChanged) {
                const auto& sensors = console->GetSensors();
                if (sensors.accelFixed) cpu.SetAcceleration(sensors.ax, sensors.ay, sensors.az);
                cpu.SetPotentiometerValue(0xFF - sensors.volume);
                sensorVersion = console->SensorVersion();
            }
        } else {
            cpu.SetPotentiometerValue(0xFF - window->GetVolumeValue()); // Update potentiometer (volume) value
        }
    };
    // With --deterministic, a script's leading commands (up to its first wait)
    // take effect before the CPU starts, so fixed sensor values reach the guest
    // at the same point of every run instead of whenever the first host poll
    // happens to land.
    if (headless && deterministic) {
        console->PollEvents();
        pushSensors();
    }

    // Start CPU execution thread
    std::thread cpuThread;
    if (positional.size() > 1) {
        cpuThread = std::thread(LdrExecutionThread, std::ref(cpu), std::ref(parser));
    } else {
        // If no LDR file is provided, just run the CPU without loading any code
        cpuThread = std::thread(BootExcutionThread, std::ref(cpu));
    }

    // Main thread handles GLFW display or headless input. cpuShouldStop is in
    // the condition so SIGTERM/SIGINT and CPU thread exit end the loop too.
    int result = 0;
    try {
        while (!cpuShouldStop.load() && !(headless ? console->ShouldClose() : window->ShouldClose())) {
            if (headless) console->PollEvents();
            else window->PollEvents();
            pushSensors();

            // Preserve the existing host frame/input cadence, without a GL swap.
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
        }
    } catch (const std::exception& e) {
        LogError("Headless input: %s", e.what());
        result = 1;
    }

    // Signal CPU thread to stop and wait for it
    LogInfo("Stopping CPU thread...");
    cpuShouldStop.store(true);
    cpuThread.join();
    if (cpu.Failed()) {
        LogError("Emulation stopped: CPU execution failed");
        result = 1;
    }

    return result;
}

int main(int argc, char* argv[]) {
    try {
        return RunEmulator(argc, argv);
    } catch (const std::exception& e) {
        LogError("%s", e.what());
        return 1;
    }
}
