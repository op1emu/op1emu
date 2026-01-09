#pragma once

#include <functional>

class Display {
public:
    virtual ~Display() {}

    virtual void Initialize(int rows, int lines) = 0;
    virtual void UpdateRowBuffer(int x, int y, const void* data, int length) = 0;
    virtual void SetOnFrameStartCallback(const std::function<void(Display&)>& callback) = 0;
};