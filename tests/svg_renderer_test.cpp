#include "svg_background_renderer.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <stdexcept>
static void Require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
static bool Rejects(const std::function<void()> &f) {
    try {
        f();
    } catch (const std::exception &) {
        return true;
    }
    return false;
}
int main() {
    Require(Rejects([] { SvgBackgroundRenderer::BufferSize(0, 10); }), "empty raster rejected");
    Require(Rejects([] { SvgBackgroundRenderer::BufferSize(UINT32_MAX, UINT32_MAX); }),
            "overflow rejected before allocation");
    Require(Rejects([] { SvgBackgroundRenderer::BufferSize(8192, 8192); }), "pixel budget enforced");
    Require(SvgBackgroundRenderer::BufferSize(20, 10) == 800, "RGBA byte count");
    Require(Rejects([] { SvgBackgroundRenderer missing("no-such-op1.svg"); }),
            "missing file/build-disabled diagnostics");
    if (!SvgBackgroundRenderer::Available()) {
        std::puts("Rust-free fallback checks passed");
        return 0;
    }
    Require(Rejects([] { SvgBackgroundRenderer malformed(OP1_TEST_DATA_DIR "/malformed.svg"); }),
            "malformed SVG rejected");
    Require(Rejects([] { SvgBackgroundRenderer invalid(OP1_TEST_DATA_DIR "/invalid-size.svg"); }),
            "invalid intrinsic size rejected");
    SvgBackgroundRenderer paint(OP1_TEST_DATA_DIR "/paint.svg");
    Require(paint.Width() == 20 && paint.Height() == 10, "intrinsic dimensions");
    for (unsigned density : {1, 2}) {
        auto p = paint.Render(20 * density, 10 * density);
        auto pixel = [&](unsigned x, unsigned y) { return p.rgba.data() + (y * p.width + x) * 4; };
        auto red = pixel(3 * density, 3 * density), green = pixel(15 * density, 3 * density);
        Require(red[0] == 255 && red[1] == 0 && red[3] == 255, "nonzero viewBox and opaque red");
        Require(green[0] == 0 && green[1] >= 127 && green[1] <= 128 && green[3] == green[1],
                "half alpha is premultiplied");
    }
    auto box = paint.Render(40, 40);
    Require(box.rgba[(5 * 40 + 5) * 4 + 3] == 0 && box.rgba[(15 * 40 + 5) * 4] == 255,
            "uniform scale with transparent letterbox");
    SvgBackgroundRenderer filter(OP1_TEST_DATA_DIR "/filters.svg");
    auto p = filter.Render(64, 32);
    const auto alpha = [&](int x, int y) { return p.rgba[(y * 64 + x) * 4 + 3]; };
    Require(alpha(10, 16) > 0 && alpha(10, 16) < alpha(16, 16),
            "Gaussian blur spreads alpha beyond source geometry");
    unsigned displaced = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 32; x < 64; ++x) {
            const bool circle = (x + .5 - 48) * (x + .5 - 48) + (y + .5 - 16) * (y + .5 - 16) < 81;
            displaced += bool(alpha(x, y) > 127) != circle;
        }
    Require(displaced > 5, "turbulence/displacement changes circle boundary");
    Require(p.rgba == filter.Render(64, 32).rgba, "seeded filters deterministic and tree reusable");
    for (size_t i = 0; i < p.rgba.size(); i += 4)
        for (int c = 0; c < 3; ++c)
            Require(p.rgba[i + c] <= p.rgba[i + 3], "filtered pixels remain premultiplied");
    std::puts("SVG paint, viewBox, alpha, filters and bounds passed");
}
