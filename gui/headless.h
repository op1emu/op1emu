#pragma once

#include "peripheral/display.h"
#include "peripheral/keyboard.h"
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <utility>

// Host frontend only: the PPI/DMA still runs, but its pixels are discarded.
// Like GLFWDisplay, frame and key callbacks enqueue work on the CPU thread.
class HeadlessFrontend : public Display, public Keyboard {
public:
    using Clock = std::chrono::steady_clock;
    HeadlessFrontend(const std::string& configPath, int inputFd);
    ~HeadlessFrontend() override;
    HeadlessFrontend(const HeadlessFrontend&) = delete;
    HeadlessFrontend& operator=(const HeadlessFrontend&) = delete;

    void Initialize(int, int) override {}
    void UpdateRowBuffer(int, int, const void*, int) override {}
    void SetOnFrameStartCallback(const std::function<void(Display&)>& callback) override {
        frameCallback_ = callback;
    }
    void PollEvents(Clock::time_point now = Clock::now());
    bool ShouldClose() const { return shouldClose_; }

private:
    bool ReadLine(std::string& line);
    void Execute(const std::string& line, Clock::time_point now);
    using Pin = std::pair<int, int>;
    std::map<std::string, Pin> keys_;
    std::function<void(Display&)> frameCallback_;
    int inputFd_ = -1;
    bool eof_ = false;
    bool shouldClose_ = false;
    std::string pending_;
    Clock::time_point resumeAt_{};
    std::optional<Pin> releaseAtResume_;
};
