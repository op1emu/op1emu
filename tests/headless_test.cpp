#include "headless.h"
#include "cpu/cpu.h"
#include "peripheral/mcp230xx.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
                            "wait", "quit extra", "unknown", "press"}) {
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
