#pragma once

#include "peripheral/display.h"
#include "peripheral/keyboard.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Host frontend only: nothing is drawn, but the PPI/DMA still runs and the
// last complete frame is kept so scripts can wait for and capture it.
// Like GLFWDisplay, frame and key callbacks enqueue work on the CPU thread.
class HeadlessFrontend : public Display, public Keyboard {
public:
    using Clock = std::chrono::steady_clock;
    HeadlessFrontend(const std::string& configPath, int inputFd);
    ~HeadlessFrontend() override;
    HeadlessFrontend(const HeadlessFrontend&) = delete;
    HeadlessFrontend& operator=(const HeadlessFrontend&) = delete;

    // Called on the CPU thread. Re-initializing with the same geometry keeps
    // the pixels: firmware rewrites PPI_CONTROL without starting a new frame.
    void Initialize(int width, int height) override;
    void UpdateRowBuffer(int x, int y, const void* data, int length) override;
    void SetOnFrameStartCallback(const std::function<void(Display&)>& callback) override {
        frameCallback_ = callback;
    }
    void PollEvents(Clock::time_point now = Clock::now());
    bool ShouldClose() const { return shouldClose_; }

    // Number of complete frames seen so far (a frame starts at pixel (0,0)).
    uint64_t FrameCount() const { return frameCount_.load(std::memory_order_acquire); }

    // Sensor state set by scripts. Main-thread only; the host loop pushes it
    // to the CPU when SensorVersion() changes.
    struct Sensors {
        bool accelFixed = false;
        int16_t ax = 0, ay = 0, az = 0;
        uint8_t volume = 128;
    };
    const Sensors& GetSensors() const { return sensors_; }
    uint64_t SensorVersion() const { return sensorVersion_; }

private:
    void WriteScreenshot(const std::string& path) const;
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

    Sensors sensors_;
    uint64_t sensorVersion_ = 0;

    std::optional<uint64_t> frameTarget_;
    Clock::time_point frameDeadline_{};

    // Frame state. Written by the CPU thread, read by the main thread.
    mutable std::mutex frameMutex_;
    int width_ = 0, height_ = 0;
    std::vector<uint16_t> work_, latest_;
    std::vector<uint8_t> seen_;
    size_t remaining_ = 0;
    bool started_ = false;
    std::atomic<uint64_t> frameCount_{0};
};
