#pragma once

// Frames as the headless frontend takes them (gui/headless.cpp): one begins
// at the origin and is complete once every pixel of the panel came. Each
// complete one goes to the host's Module.onFrame(pointer, width, height):
// width x height RGB565 pixels in the module's memory, valid until the next
// call (the panel is drawn rotated a quarter turn, as gui/ui.json places it).

#include "peripheral/display.h"

#include <emscripten.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

EM_JS(void, op1_js_frame, (const uint16_t* pixels, int width, int height), {
    if (Module.onFrame) Module.onFrame(pixels >>> 0, width, height);
});

class WebDisplay final : public Display {
public:
    void Initialize(int width, int height) override {
        if (width == width_ && height == height_) return;
        width_ = std::max(width, 0);
        height_ = std::max(height, 0);
        work_.assign(size_t(width_) * height_, 0);
        seen_.assign(work_.size(), 0);
        latest_.clear();
        remaining_ = work_.size();
        started_ = false;
    }

    void UpdateRowBuffer(int x, int y, const void* data, int length) override {
        if (!data || length <= 0 || length % 2 || x < 0 || y < 0 || y >= height_ || x >= width_ ||
            length / 2 > width_ - x)
            return;
        if (x == 0 && y == 0) {
            started_ = true;
            remaining_ = work_.size();
            std::fill(seen_.begin(), seen_.end(), 0);
        }
        if (!started_) return;
        const size_t begin = size_t(y) * width_ + x, count = length / 2;
        std::memcpy(work_.data() + begin, data, length);
        for (size_t i = begin; i < begin + count; ++i) {
            if (!seen_[i]) {
                seen_[i] = 1;
                --remaining_;
            }
        }
        if (remaining_ == 0) {
            latest_ = work_;
            started_ = false;
            ++frames_;
            op1_js_frame(latest_.data(), width_, height_);
        }
    }

    uint64_t Frames() const { return frames_; }

private:
    int width_ = 0, height_ = 0;
    std::vector<uint16_t> work_, latest_;
    std::vector<uint8_t> seen_;
    size_t remaining_ = 0;
    bool started_ = false;
    uint64_t frames_ = 0;
};
