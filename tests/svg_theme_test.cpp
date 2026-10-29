#include "svg_background_renderer.h"
#include "svg_theme.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
static const uint8_t *Pixel(const SvgPixels &p, unsigned x, unsigned y, unsigned scale) {
    return p.rgba.data() + (size_t(y * scale) * p.width + x * scale) * 4;
}
int main() {
    const std::string root = OP1_SOURCE_DIR;
    std::filesystem::current_path(root);
    Require(Rejects([] { SvgTheme::Load("no-such-theme.json"); }), "missing theme rejected");
    Require(Rejects([] { SvgTheme::Load("tests/fixtures/svg/malformed.svg"); }), "malformed JSON rejected");
    // Validate real custom-file input, including CSS injection, missing roles and versions.
    const auto temp = std::filesystem::temp_directory_path() /
                      ("op1-theme-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    for (
        const auto *bad :
        {R"({"version":2,"name":"bad","colors":{}})",
         R"({"version":1,"name":"bad","colors":{"body":"#123456"}})",
         R"({"version":1,"name":"bad","colors":{"body":"red; } rect { fill: red","panel":"#123456","keycap":"#123456","encoder-ring":"#123456"}})"}) {
        {
            std::ofstream f(temp);
            f << bad;
        }
        Require(Rejects([&] { SvgTheme::Load(temp.string()); }), "invalid palette rejected");
    }
    std::filesystem::remove(temp);
    auto independent = SvgTheme::Load(SvgTheme::Resolve("silver"));
    independent.colors["body"] = "#ff0000";
    independent.colors["panel"] = "#00ff00";
    independent.colors["keycap"] = "#0000ff";
    independent.colors["encoder-ring"] = "#ffff00";
    if (!SvgBackgroundRenderer::Available()) {
        for (const auto *name : {"slate", "silver", "yellow", "green", "purple", "orange"})
            Require(!SvgTheme::Load(SvgTheme::Resolve(name)).Stylesheet().empty(),
                    "Rust-free theme validation");
        std::puts("Theme validation passed without Rust/resvg");
        return 0;
    }
    SvgBackgroundRenderer source(root + "/gui/op1.svg");
    SvgBackgroundRenderer tagged(root + "/gui/op1-themeable.svg");
    SvgBackgroundRenderer separate(root + "/gui/op1-themeable.svg", independent.Stylesheet());
    const auto independentPixels = separate.Render(1204, 434);
    const auto body = Pixel(independentPixels, 1170, 200, 1);
    const auto panel = Pixel(independentPixels, 560, 33, 1);
    const auto key = Pixel(independentPixels, 310, 350, 1);
    const auto ring = Pixel(independentPixels, 637, 84, 1);
    Require(body[0] > 200 && body[1] < 30 && body[2] < 30, "body role is independent");
    Require(panel[1] > panel[0] * 2 && panel[1] > panel[2] * 2, "panel role is independent");
    Require(key[2] > key[0] * 2 && key[2] > key[1] * 2, "keycap role is independent");
    Require(ring[0] > 100 && ring[1] > 100 && ring[2] < 30, "encoder-ring role is independent");
    for (const unsigned scale : {1, 2}) {
        const auto original = source.Render(1204 * scale, 434 * scale);
        Require(original.rgba == tagged.Render(original.width, original.height).rgba,
                "class-only derivative is pixel-identical to original at 1x/2x");
        for (const auto *name : {"slate", "silver", "yellow", "green", "purple", "orange"}) {
            SvgBackgroundRenderer renderer(root + "/gui/op1-themeable.svg",
                                           SvgTheme::Load(SvgTheme::Resolve(name)).Stylesheet());
            Require(renderer.Width() == source.Width() && renderer.Height() == source.Height(),
                    "theme retains intrinsic geometry");
            const auto pixels = renderer.Render(original.width, original.height);
            if (std::string(name) == "silver")
                Require(pixels.rgba == original.rgba, "silver theme exactly matches original artwork");
            else
                Require(pixels.rgba != original.rgba, "colored palette actually changes rendered output");
            for (size_t i = 3; i < pixels.rgba.size(); i += 4)
                Require(pixels.rgba[i] == original.rgba[i], "themes preserve the full alpha silhouette");
            // Screen and encoder caps have independent paint, including the white cap.
            for (const auto &point :
                 {std::pair<unsigned, unsigned>{410, 80}, {608, 82}, {738, 82}, {869, 82}, {1000, 82}}) {
                const auto a = Pixel(original, point.first, point.second, scale);
                const auto b = Pixel(pixels, point.first, point.second, scale);
                for (unsigned c = 0; c < 4; ++c)
                    Require(a[c] == b[c], "screen and encoder-cap paint preserved");
            }
            // Texture and translucent edge filters can reflect a little surrounding panel color.
            const auto blackKey = Pixel(pixels, 275, 249, scale);
            Require(blackKey[0] < 45 && blackKey[1] < 45 && blackKey[2] < 45,
                    "black keys retain their dark centers");
        }
    }
    std::puts(
        "Six themes: validation, independent paints, exact silver, 1x/2x geometry, alpha and accents passed");
}
