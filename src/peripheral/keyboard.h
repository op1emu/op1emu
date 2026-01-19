#pragma once

#include <functional>

class Keyboard {
public:
    virtual ~Keyboard() {}

    void SetKeyEventCallback(const std::function<void(int, int, bool)>& callback) {
        keyEventCallback = callback;
    }

    virtual void OnKeyPressed(int bank, int index) const { keyEventCallback(bank, index, true); }
    virtual void OnKeyReleased(int bank, int index) const { keyEventCallback(bank, index, false); }

protected:
    std::function<void(int, int, bool)> keyEventCallback;
};