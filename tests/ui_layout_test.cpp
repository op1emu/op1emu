#include "ui_layout.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
static void Require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
int main() {
    auto png = UILayout::Load(OP1_SOURCE_DIR "/gui/ui.json"),
         svg = UILayout::Load(OP1_SOURCE_DIR "/gui/ui-svg.json");
    Require(!png.svg && svg.svg && png.buttons.size() == svg.buttons.size(), "both complete layouts load");
    std::map<std::string, UIGpio> wiring;
    for (auto &b : png.buttons)
        wiring[b.name] = b.gpio;
    for (auto &b : svg.buttons)
        Require(wiring.at(b.name) == b.gpio, "all buttons preserve GPIO wiring");
    for (size_t i = 0; i < png.encoders.size(); ++i)
        Require(png.encoders[i].a == svg.encoders[i].a && png.encoders[i].b == svg.encoders[i].b,
                "encoder wiring unchanged");
    Require(svg.buttons.size() == 58 && svg.encoders.size() == 4, "full panel control count");
    auto fit = UIFit::Into(1204, 434, 800, 600);
    auto p = fit.ToArt({fit.x + 607.542 * fit.scale, fit.y + 84.2573 * fit.scale});
    Require(std::abs(p.x - 607.542) < 1e-8 && std::abs(p.y - 84.2573) < 1e-8,
            "logical cursor inverse fit with letterbox");
    // Device density changes physical raster size only; cursor coords stay logical.
    for (int d : {1, 2})
        Require(std::abs(fit.scale * d * 1204 - 800 * d) < 1e-8, "Retina physical size");
    const std::array<UIPoint, 4> skew = {UIPoint{10, 20}, {210, 40}, {190, 130}, {30, 110}};
    auto m = UIScreenMatrix(skew);
    int i = 0;
    for (auto uv : {UIPoint{0, 0}, {1, 0}, {1, 1}, {0, 1}}) {
        double w = m[3] * uv.x + m[7] * uv.y + 1;
        Require(std::abs((m[0] * uv.x + m[4] * uv.y + m[12]) / w - skew[i].x) < 1e-8 &&
                    std::abs((m[1] * uv.x + m[5] * uv.y + m[13]) / w - skew[i].y) < 1e-8,
                "projective corners match");
        ++i;
    }
    UIRegion polygon{{{0, 0}, {10, 2}, {8, 10}, {0, 8}}};
    Require(polygon.Contains({5, 5}) && !polygon.Contains({9, 9}), "polygon hit testing");
    UIPanelInput input(svg);
    std::vector<std::tuple<int, int, bool>> events;
    input.event = [&](int bank, int pin, bool down) { events.emplace_back(bank, pin, down); };
    for (auto &b : svg.buttons) {
        UIPoint center{};
        for (auto p : b.region.polygon) {
            center.x += p.x / 4;
            center.y += p.y / 4;
        }
        events.clear();
        input.Mouse(true, center);
        input.Mouse(false, {-100, -100});
        Require(events == std::vector<std::tuple<int, int, bool>>{{b.gpio.first, b.gpio.second, true},
                                                                  {b.gpio.first, b.gpio.second, false}},
                "every button presses and releases outside");
    }
    events.clear();
    input.Mouse(true, {182, 379});
    input.Key(340, {5, 6}, true);
    input.Mouse(false, {});
    Require(events.size() == 1 && std::get<2>(events[0]), "mouse release preserves keyboard-held shift");
    input.ReleaseAll();
    Require(events.size() == 2 && !std::get<2>(events[1]), "focus loss releases held keys");
    for (auto &e : svg.encoders) {
        events.clear();
        auto p = e.counterClockwise.polygon[0];
        p.x += 2;
        p.y += 2;
        input.Mouse(true, p);
        Require(e.phase == 3, "counterclockwise wraps 0 to 3");
        input.Mouse(false, {});
        p = e.clockwise.polygon[0];
        p.x += 2;
        p.y += 2;
        input.Mouse(true, p);
        Require(e.phase == 0, "clockwise returns to 0");
        Require(events.size() == 4, "both quadrature lines updated each step");
    }
    for (int i = 0; i < 60; ++i)
        input.Mouse(true, {200, 50});
    Require(input.Volume() == 255, "volume saturates high");
    for (int i = 0; i < 60; ++i)
        input.Mouse(true, {160, 50});
    Require(input.Volume() == 0, "volume saturates low");
    std::puts("Panel wiring, transforms and input passed");
}
