#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Pixels always use resvg's premultiplied RGBA8888, top row first.
struct SvgPixels {
    uint32_t width, height;
    std::vector<uint8_t> rgba;
};
class SvgBackgroundRenderer {
  public:
    explicit SvgBackgroundRenderer(const std::string &path, const std::string &stylesheet = "");
    ~SvgBackgroundRenderer();
    SvgBackgroundRenderer(const SvgBackgroundRenderer &) = delete;
    SvgBackgroundRenderer &operator=(const SvgBackgroundRenderer &) = delete;
    float Width() const;
    float Height() const;
    SvgPixels Render(uint32_t width, uint32_t height) const;
    static bool Available();
    static size_t BufferSize(uint32_t width, uint32_t height);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
