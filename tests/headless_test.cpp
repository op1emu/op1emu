#include "headless.h"
#include "cpu/cpu.h"
#include "peripheral/mcp230xx.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <unistd.h>
#include <vector>

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct Pipe {
    int fds[2];
    Pipe() { Require(pipe(fds) == 0, "create pipe"); }
    ~Pipe() { close(fds[0]); if (fds[1] >= 0) close(fds[1]); }
    void Send(const std::string& text) {
        Require(write(fds[1], text.data(), text.size()) == static_cast<ssize_t>(text.size()), "write pipe");
    }
    void End() { close(fds[1]); fds[1] = -1; }
};

// Exercise the real attachment and event queue without executing firmware.
// Only one CPU exists in this process (bcore's CEC state is process-global).
class TestCpu : public BlackFinCpu {
public:
    void Drain() { ProcessEvents(); ProcessEvents(); }
    GPIOPinLevel Pin(int bank, int index) { return gpioExpanders.at(bank)->GetPinOutput(index); }
    void EnablePlayInterrupt() {
        const u8 command[] = {0x04, 1 << 2}; // MCP23017 GPINTENA, play pin
        Require(gpioExpanders[5]->Write(command, sizeof(command)), "enable key interrupt over I2C");
        gpioExpanders[5]->Stop();
    }
    void CheckFramePulse(HeadlessFrontend& frontend, HeadlessFrontend::Clock::time_point now) {
        portG->Write32(0x40, 1 << 3); // enable frame-sync input
        portG->SetPinInput(3, GPIOPinLevel::High);
        frontend.PollEvents(now);
        Drain();
        Require(portG->GetPinOutput(3) == GPIOPinLevel::Low, "headless frame starts PORTG3 pulse");
        for (int i = 0; i < 1000; ++i) ProcessEvents();
        Require(portG->GetPinOutput(3) == GPIOPinLevel::High, "CPU queue completes delayed frame pulse");
    }
};

// A frame of `width` x `height` pixels, written one row at a time like the PPI DMA.
static uint16_t PixelAt(int x, int y) { return static_cast<uint16_t>(1 + y * 4 + x); } // R field only
static void FeedRows(HeadlessFrontend& frontend, int width, int from, int to) {
    for (int y = from; y < to; ++y) {
        std::vector<uint16_t> row(width);
        for (int x = 0; x < width; ++x) row[x] = static_cast<uint16_t>(PixelAt(x, y) << 11);
        frontend.UpdateRowBuffer(0, y, row.data(), width * 2);
    }
}
static std::string ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main() {
    const auto config = std::filesystem::absolute("gui/ui.json").string();
    using Clock = HeadlessFrontend::Clock;
    using namespace std::chrono_literals;
    const auto start = Clock::now();
    using Event = std::tuple<int, int, bool>;
    {
        Pipe pipe;
        HeadlessFrontend frontend(config, pipe.fds[0]);
        std::vector<Event> events;
        int frames = 0;
        frontend.SetKeyEventCallback([&](int bank, int pin, bool pressed) { events.emplace_back(bank, pin, pressed); });
        frontend.SetOnFrameStartCallback([&](Display&) { ++frames; });
        frontend.Initialize(320, 160);
        frontend.UpdateRowBuffer(0, 0, nullptr, 0);
        pipe.Send("# comment\r\npress shift\nwait 50\ntap f3# 100\nrelease shift\nquit");
        pipe.End();
        frontend.PollEvents(start);
        Require(events == std::vector<Event>{{5, 6, true}}, "press maps to shared UI wiring");
        frontend.PollEvents(start + 49ms);
        Require(events.size() == 1 && frames == 2, "wait preserves frame sync without executing next command");
        frontend.PollEvents(start + 50ms);
        Require(events.back() == Event{3, 13, true}, "sharp note is not a comment");
        frontend.PollEvents(start + 149ms);
        Require(events.size() == 2, "tap holds for requested duration");
        frontend.PollEvents(start + 150ms);
        Require(events.back() == Event{3, 13, false} && !frontend.ShouldClose(), "tap releases before following commands");
        frontend.PollEvents(start + 166ms);
        Require(events.back() == Event{5, 6, false} && frontend.ShouldClose(), "combo release and final unterminated quit");
    }
    {
        Pipe pipe;
        HeadlessFrontend frontend(config, pipe.fds[0]);
        int keys = 0, frames = 0;
        frontend.SetKeyEventCallback([&](int, int, bool) { ++keys; });
        frontend.SetOnFrameStartCallback([&](Display&) { ++frames; });
        pipe.Send("tap pl");
        frontend.PollEvents(start);
        Require(keys == 0 && frames == 1, "partial line does not block or execute");
        pipe.Send("ay\n");
        frontend.PollEvents(start + 16ms);
        pipe.End();
        frontend.PollEvents(start + 115ms);
        Require(keys == 1, "default tap holds for 100ms");
        frontend.PollEvents(start + 116ms);
        frontend.PollEvents(start + 132ms);
        Require(keys == 2 && !frontend.ShouldClose(), "EOF preserves timed release and keeps emulator alive");
    }
    for (const auto& bad : {"press missing", "release play extra", "tap play 0", "tap play -1",
                            "tap play 1.5", "tap play 86400001", "wait nan", "wait 999999999999999999999",
                            "wait", "quit extra", "unknown", "press",
                            "accel", "accel 1 2", "accel 1 2 3 4", "accel random 1", "accel 32768 0 0",
                            "accel 0 -32769 0", "accel a b c", "accel 1.5 0 0", "accel +1 0 0",
                            "volume", "volume 256", "volume -1", "volume 1 2", "volume 0x10",
                            "wait-frames", "wait-frames 0", "wait-frames -1", "wait-frames 100001",
                            "wait-frames 1 0", "wait-frames 1 2 3", "wait-frames x",
                            "screenshot", "screenshot a b", "screenshot /tmp/op1-no-frame.ppm"}) {
        Pipe pipe;
        HeadlessFrontend frontend(config, pipe.fds[0]);
        int keys = 0;
        frontend.SetKeyEventCallback([&](int, int, bool) { ++keys; });
        pipe.Send(std::string(bad) + "\n");
        bool rejected = false;
        try { frontend.PollEvents(start); } catch (const std::exception&) { rejected = true; }
        Require(rejected && keys == 0, "invalid command rejected before key side effects");
    }
    {
        Pipe pipe;
        HeadlessFrontend frontend(config, pipe.fds[0]);
        pipe.Send(std::string(4097, 'x'));
        frontend.PollEvents(start);
        bool rejected = false;
        try { frontend.PollEvents(start); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "overlong unterminated input is bounded");
    }
    {
        Pipe pipe;
        HeadlessFrontend frontend(config, pipe.fds[0]);
        Require(!frontend.GetSensors().accelFixed && frontend.GetSensors().volume == 128 &&
                    frontend.SensorVersion() == 0, "sensors default to random acceleration and mid volume");
        pipe.Send("accel 64 -64 512\nvolume 200\n");
        frontend.PollEvents(start);
        const auto& sensors = frontend.GetSensors();
        Require(sensors.accelFixed && sensors.ax == 64 && sensors.ay == -64 && sensors.az == 512 &&
                    sensors.volume == 200 && frontend.SensorVersion() == 2, "accel and volume set sensors");
        pipe.Send("accel -32768 32767 0\naccel random\nvolume 0\n");
        frontend.PollEvents(start + 16ms);
        Require(!sensors.accelFixed && sensors.volume == 0 && frontend.SensorVersion() == 5,
                "accel random resumes random feed; int16 limits and volume 0 accepted");
    }
    {
        Pipe pipe;
        HeadlessFrontend frontend(config, pipe.fds[0]);
        frontend.Initialize(4, 2);
        FeedRows(frontend, 4, 1, 2); // the tail of a frame we did not see from the start
        Require(frontend.FrameCount() == 0, "rows before the frame origin never complete a frame");
        FeedRows(frontend, 4, 0, 1);
        Require(frontend.FrameCount() == 0, "half a frame is not a frame");
        frontend.Initialize(4, 2); // firmware rewrites PPI_CONTROL: same geometry keeps pixels
        FeedRows(frontend, 4, 1, 2);
        Require(frontend.FrameCount() == 1, "same-geometry Initialize does not discard a frame in progress");
        FeedRows(frontend, 4, 1, 2);
        Require(frontend.FrameCount() == 1, "rows after completion wait for the next frame origin");
        const std::string path = std::string("/tmp/op1-headless-shot-") + std::to_string(getpid()) + ".ppm";
        pipe.Send("screenshot " + path + "\n");
        frontend.PollEvents(start);
        // 4x2 portrait rotated 90 degrees clockwise is 2x4: output (xd, yd) = source (yd, 1 - xd).
        const std::string ppm = ReadFile(path);
        const std::string header = "P6\n2 4\n255\n";
        Require(ppm.size() == header.size() + 2 * 4 * 3 && ppm.compare(0, header.size(), header) == 0,
                "screenshot is a 2x4 binary PPM");
        for (int yd = 0; yd < 4; ++yd) {
            for (int xd = 0; xd < 2; ++xd) {
                const unsigned r = PixelAt(yd, 1 - xd);
                const auto* out = reinterpret_cast<const uint8_t*>(ppm.data()) + header.size() + (yd * 2 + xd) * 3;
                Require(out[0] == ((r << 3) | (r >> 2)) && out[1] == 0 && out[2] == 0,
                        "screenshot rotates clockwise and expands RGB565");
            }
        }
        std::filesystem::remove(path);
        pipe.Send("screenshot /nonexistent-op1-dir/x.ppm\n");
        bool rejected = false;
        try { frontend.PollEvents(start + 16ms); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "screenshot to an unwritable path fails loudly");
    }
    {
        // Full-scale colors survive the RGB565 -> RGB888 expansion.
        Pipe pipe;
        HeadlessFrontend frontend(config, pipe.fds[0]);
        frontend.Initialize(1, 2);
        const uint16_t pixels[2] = {0xFFFF, 0x07E0}; // white, pure green
        frontend.UpdateRowBuffer(0, 0, &pixels[0], 2);
        frontend.UpdateRowBuffer(0, 1, &pixels[1], 2);
        const std::string path = std::string("/tmp/op1-headless-color-") + std::to_string(getpid()) + ".ppm";
        pipe.Send("screenshot " + path + "\n");
        frontend.PollEvents(start);
        const std::string ppm = ReadFile(path);
        const std::string header = "P6\n2 1\n255\n";
        const auto* out = reinterpret_cast<const uint8_t*>(ppm.data()) + header.size();
        Require(ppm.size() == header.size() + 6 && out[0] == 0 && out[1] == 255 && out[2] == 0 &&
                    out[3] == 255 && out[4] == 255 && out[5] == 255,
                "white and green expand to full-scale channels");
        std::filesystem::remove(path);
    }
    {
        Pipe pipe;
        HeadlessFrontend frontend(config, pipe.fds[0]);
        frontend.Initialize(4, 2);
        int frames = 0;
        frontend.SetOnFrameStartCallback([&](Display&) { ++frames; });
        pipe.Send("wait-frames 2\nvolume 7\n");
        frontend.PollEvents(start);
        FeedRows(frontend, 4, 0, 2);
        frontend.PollEvents(start + 16ms);
        Require(frontend.SensorVersion() == 0 && frames == 2, "wait-frames holds later commands, frame sync continues");
        FeedRows(frontend, 4, 0, 2);
        frontend.PollEvents(start + 32ms);
        Require(frontend.SensorVersion() == 1 && frontend.GetSensors().volume == 7, "wait-frames resumes after N new frames");
    }
    {
        Pipe pipe;
        HeadlessFrontend frontend(config, pipe.fds[0]);
        frontend.Initialize(4, 2);
        FeedRows(frontend, 4, 0, 2); // an older frame does not count
        pipe.Send("wait-frames 1 100\nvolume 7\n");
        frontend.PollEvents(start);
        frontend.PollEvents(start + 99ms);
        Require(frontend.SensorVersion() == 0, "wait-frames ignores frames from before the command");
        bool timedOut = false;
        try { frontend.PollEvents(start + 100ms); } catch (const std::exception&) { timedOut = true; }
        Require(timedOut, "wait-frames times out instead of hanging");
    }
    // Isolate OTP creation performed by BlackFinCpu's constructor.
    char directory[] = "/tmp/op1-headless-test-XXXXXX";
    Require(mkdtemp(directory) != nullptr, "create isolated CPU directory");
    auto original = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    {
        TestCpu cpu;
        Pipe pipe;
        auto frontend = std::make_shared<HeadlessFrontend>(config, pipe.fds[0]);
        cpu.AttachDisplay(frontend);
        cpu.AttachKeyboard(frontend);
        pipe.Send("release play\n");
        frontend->PollEvents(start);
        cpu.Drain();
        Require(cpu.Pin(5, 2) == GPIOPinLevel::High, "release reaches expander");
        cpu.EnablePlayInterrupt();
        Require(cpu.Pin(5, 16) == GPIOPinLevel::High, "key interrupt initially inactive");
        pipe.Send("tap play 100\n");
        frontend->PollEvents(start + 16ms);
        Require(cpu.Pin(5, 2) == GPIOPinLevel::High, "frontend does not mutate GPIO on caller thread");
        cpu.Drain();
        Require(cpu.Pin(5, 2) == GPIOPinLevel::Low, "CPU queue applies active-low press");
        Require(cpu.Pin(5, 16) == GPIOPinLevel::Low, "press asserts expander interrupt output");
        frontend->PollEvents(start + 116ms);
        cpu.Drain();
        Require(cpu.Pin(5, 2) == GPIOPinLevel::High, "CPU queue applies timed release");
        cpu.CheckFramePulse(*frontend, start + 132ms);
    }
    std::filesystem::current_path(original);
    std::filesystem::remove_all(directory);
    std::puts("Headless frontend and CPU input integration tests passed");
}
