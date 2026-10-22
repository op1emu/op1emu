#pragma once

#include <cstdint>
#include <functional>

class Keyboard {
public:
    virtual ~Keyboard() {}

    void SetKeyEventCallback(const std::function<void(int, int, bool)>& callback) {
        keyEventCallback = callback;
    }

    // A key event for guest time delayNs after the CPU next looks (one queued
    // before the CPU starts is exactly that far into the run). Without a timed
    // callback, at once.
    void SetTimedKeyEventCallback(const std::function<void(int, int, bool, uint64_t)>& callback) {
        timedKeyEventCallback = callback;
    }

    virtual void OnKeyPressed(int bank, int index) const { keyEventCallback(bank, index, true); }
    virtual void OnKeyReleased(int bank, int index) const { keyEventCallback(bank, index, false); }
    void OnKeyAt(int bank, int index, bool pressed, uint64_t delayNs) const {
        if (timedKeyEventCallback) timedKeyEventCallback(bank, index, pressed, delayNs);
        else keyEventCallback(bank, index, pressed);
    }

protected:
    std::function<void(int, int, bool)> keyEventCallback;
    std::function<void(int, int, bool, uint64_t)> timedKeyEventCallback;
};