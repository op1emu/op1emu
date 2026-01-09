#include "timer.h"

Timer::Timer()
{
    running = true;
    thread = std::thread([this] {
        auto now = std::chrono::steady_clock::now();
        while (running) {
            std::unique_lock<std::mutex> lock(mutex);
            if (!enabled) {
                wakeup.wait(lock, [this]() { return enabled || !running; });
                now = std::chrono::steady_clock::now();
            }
            if (enabled) {
                working = true;
                wakeup.wait_until(lock, now + interval);
                now = std::chrono::steady_clock::now();
                callback(*this);
                if (oneshot) {
                    enabled = false;
                }
                working = false;
                wakeup.notify_all();
            }
        }
    });
}

Timer::~Timer()
{
    if (running) {
        running = false;
        enabled = false;
        wakeup.notify_all();
        thread.join();
    }
}

void Timer::SetOneshot(bool oneshot)
{
    std::unique_lock<std::mutex> lock(mutex);
    this->oneshot = oneshot;
}

void Timer::SetInterval(std::chrono::microseconds interval, std::function<void(Timer&)> callback)
{
    std::unique_lock<std::mutex> lock(mutex);
    this->callback = callback;
    this->interval = interval;
}

void Timer::Start()
{
    std::unique_lock<std::mutex> lock(mutex);
    if (!enabled) {
        enabled = true;
        wakeup.notify_all();
    }
}

void Timer::Stop()
{
    std::unique_lock<std::mutex> lock(mutex);
    enabled = false;
    wakeup.wait(lock, [this]() { return !working; });
}