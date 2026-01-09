#pragma once

#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <condition_variable>

class Timer {
public:
    Timer();
    ~Timer();
    void SetInterval(std::chrono::microseconds interval, std::function<void(Timer&)> callback);
    void SetOneshot(bool oneshot);
    void Start();
    void Stop();

private:
    std::atomic<bool> running{false};
    std::atomic<bool> enabled{false};
    std::atomic<bool> working{false};
    bool oneshot = false;
    std::function<void(Timer&)> callback;
    std::chrono::microseconds interval;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wakeup;
};