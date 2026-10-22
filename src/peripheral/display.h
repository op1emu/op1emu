#pragma once

class Display {
public:
    virtual ~Display() {}

    virtual void Initialize(int rows, int lines) = 0;
    virtual void UpdateRowBuffer(int x, int y, const void* data, int length) = 0;
};