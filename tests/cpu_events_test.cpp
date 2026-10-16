// The CPU's event queue runs on guest time: an event runs at the first
// ProcessEvents() whose guest time has reached its due time, events due
// together in the order they were queued.
#include "cpu/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

class TestCpu : public BlackFinCpu {
public:
    TestCpu() : BlackFinCpu(true) { cpuThread_.store(std::this_thread::get_id()); }
    void Advance(uint64_t packets) { time_.Retire(packets); }
    using BlackFinCpu::ProcessEvents;
};

int main() {
    // Isolate OTP creation performed by BlackFinCpu's constructor.
    char directory[] = "/tmp/op1-cpu-events-test-XXXXXX";
    Require(mkdtemp(directory) != nullptr, "create isolated CPU directory");
    auto original = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    {
        TestCpu cpu;
        std::vector<std::string> ran;
        auto log = [&ran](const char* name) { return [&ran, name] { ran.push_back(name); }; };
        using std::chrono::nanoseconds;

        cpu.QueueEvent(log("a"), nanoseconds(10));
        cpu.QueueEvent(log("b"), nanoseconds(10));
        cpu.QueueEvent(log("c"), nanoseconds(5));
        cpu.ProcessEvents();
        Require(ran.empty(), "nothing is due at guest time 0");
        cpu.Advance(1);  // 2 ns
        cpu.ProcessEvents();
        Require(ran.empty(), "an event 5 ns away is not due after 2 ns");
        cpu.Advance(1);  // 5 ns
        cpu.ProcessEvents();
        Require(ran == std::vector<std::string>{"c"}, "the 5 ns event runs at 5 ns");
        cpu.Advance(2);  // 10 ns
        cpu.ProcessEvents();
        Require((ran == std::vector<std::string>{"c", "a", "b"}), "events due together run in queue order");

        ran.clear();
        cpu.QueueEvent([&] {
            ran.push_back("outer");
            cpu.QueueEvent(log("now"), nanoseconds(0));
            cpu.QueueEvent(log("later"), nanoseconds(1));
        }, nanoseconds(0));
        cpu.ProcessEvents();
        Require((ran == std::vector<std::string>{"outer", "now"}),
                "an event queued for now from an event runs in the same call");
        cpu.Advance(1);
        cpu.ProcessEvents();
        Require((ran == std::vector<std::string>{"outer", "now", "later"}), "and one queued for later after it");

        // Another thread's event is timed from the CPU thread's next call.
        ran.clear();
        std::thread([&] { cpu.QueueEvent(log("key"), nanoseconds(4)); }).join();
        cpu.Advance(100);
        cpu.ProcessEvents();
        Require(ran.empty(), "another thread's delay starts when the CPU thread takes the event");
        cpu.Advance(1);  // 2.5 ns later: not yet
        cpu.ProcessEvents();
        Require(ran.empty(), "4 ns have not passed");
        cpu.Advance(1);
        cpu.ProcessEvents();
        Require(ran == std::vector<std::string>{"key"}, "the event runs 4 ns after it was taken");
    }
    std::filesystem::current_path(original);
    std::filesystem::remove_all(directory);
    std::puts("cpu-events-test: ok");
}
