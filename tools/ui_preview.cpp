#include "glfw_display.h"
#include "svg_theme.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <tuple>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// This harness never constructs a CPU or opens NAND/OTP/audio. The diagnostic
// framebuffer tests the very same RGB565 upload/rotation path firmware uses.
class UIPreview {
  public:
    static void Require(bool ok, const char *message) {
        if (!ok)
            throw std::runtime_error(message);
    }
    static void Feed(GLFWDisplay &d, int width, int height) {
        d.Initialize(width, height);
        for (int y = 0; y < height; ++y) {
            std::vector<uint16_t> row(width);
            for (int x = 0; x < width; ++x)
                row[x] =
                    y < height / 2 ? (x < width / 2 ? 0xf800 : 0x07e0) : (x < width / 2 ? 0x001f : 0xffff);
            d.UpdateRowBuffer(0, y, row.data(), width * 2);
        }
    }
    static void ClickThemeButton(GLFWDisplay &d) {
        int width, height;
        glfwGetWindowSize(d.window_, &width, &height);
        const auto fit = d.PanelFit(width, height);
        const double x = fit.x + (d.artWidth_ + 32) * fit.scale, y = fit.y + 28 * fit.scale;
        Require(d.ThemeButtonContains(x, y) && fit.ToArt({x, y}).x > d.artWidth_,
                "dropdown button is outside the SVG body");
        GLFWDisplay::CursorPositionCallback(d.window_, x, y);
        GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
        GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    }
    static std::vector<uint8_t> Read(GLFWDisplay &d, int &w, int &h) {
        glfwGetFramebufferSize(d.window_, &w, &h);
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // Read the rendered back buffer before presentation so capture does
        // not depend on virtual-display/front-buffer behavior after a swap.
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        Require(glGetError() == GL_NO_ERROR, "OpenGL error during capture");
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w * 4; ++x)
                std::swap(rgba[size_t(y) * w * 4 + x], rgba[size_t(h - 1 - y) * w * 4 + x]);
        return rgba;
    }
    static void Check(GLFWDisplay &d) {
        int lw, lh, w, h;
        glfwGetWindowSize(d.window_, &lw, &lh);
        auto pixels = Read(d, w, h);
        auto fit = d.PanelFit(lw, lh);
        auto matrix = UIScreenMatrix(d.uiConfig_.screen);
        int index = 0;
        // Clockwise rotation maps source BL/TL/BR/TR to screen TL/TR/BL/BR.
        const int expected[4][3] = {{0, 0, 255}, {255, 0, 0}, {255, 255, 255}, {0, 255, 0}};
        for (auto p : {UIPoint{.25, .25}, {.75, .25}, {.25, .75}, {.75, .75}}) {
            double q = matrix[3] * p.x + matrix[7] * p.y + 1;
            int x = int((fit.x + fit.scale * (matrix[0] * p.x + matrix[4] * p.y + matrix[12]) / q) * w / lw);
            int y = int((fit.y + fit.scale * (matrix[1] * p.x + matrix[5] * p.y + matrix[13]) / q) * h / lh);
            auto pixel = pixels.data() + (size_t(y) * w + x) * 4;
            if (std::abs(int(pixel[0]) - expected[index][0]) >= 3 ||
                std::abs(int(pixel[1]) - expected[index][1]) >= 3 ||
                std::abs(int(pixel[2]) - expected[index][2]) >= 3)
                std::cerr << "screen sample " << index << " at " << x << ',' << y << " (" << w << 'x' << h
                          << ") = " << int(pixel[0]) << ',' << int(pixel[1]) << ',' << int(pixel[2]) << '\n';
            for (int c = 0; c < 3; ++c)
                Require(std::abs(int(pixels[(size_t(y) * w + x) * 4 + c]) - expected[index][c]) < 3,
                        "RGB565 screen rotation/corner mismatch");
            ++index;
        }
    }
    static std::array<uint8_t, 3> ScreenCenter(GLFWDisplay &d) {
        int lw, lh, w, h;
        glfwGetWindowSize(d.window_, &lw, &lh);
        auto pixels = Read(d, w, h);
        auto fit = d.PanelFit(lw, lh);
        auto m = UIScreenMatrix(d.uiConfig_.screen);
        const double q = (m[3] + m[7]) * .5 + 1;
        const int x = int((fit.x + fit.scale * ((m[0] + m[4]) * .5 + m[12]) / q) * w / lw);
        const int y = int((fit.y + fit.scale * ((m[1] + m[5]) * .5 + m[13]) / q) * h / lh);
        Require(x >= 0 && x < w && y >= 0 && y < h, "screen center lies inside drawable");
        auto p = pixels.data() + (size_t(y) * w + x) * 4;
        return {p[0], p[1], p[2]};
    }
    static int Run(int argc, char **argv, const std::string &theme) {
        bool test = argc == 2 && (std::string(argv[1]) == "--test" || std::string(argv[1]) == "--test-floating");
        bool floating = argc > 1 && (std::string(argv[1]) == "--floating" || std::string(argv[1]) == "--test-floating");
        const int first = floating && !test ? 2 : 1;
        std::string path = test ? (SvgBackgroundRenderer::Available() ? "gui/ui-svg.json" : "gui/ui.json")
                                : (argc > first ? argv[first] : (floating ? "gui/ui-svg.json" : "gui/ui.json"));
        GLFWDisplay d(path, floating, theme);
        Feed(d, 201, 400); // odd-width upload exercises UNPACK_ALIGNMENT=1
        glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 31);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        const auto draw = [&] {
            glfwPollEvents();
            // Acquire a drawable at the new window size before rendering. In
            // particular GLX may defer reallocating swap buffers until a swap.
            glfwSwapBuffers(d.window_);
            d.RenderFramebuffer(false);
        };
        draw();
        std::this_thread::sleep_for(std::chrono::milliseconds(140));
        draw();
        if (test) {
            GLint alignment, src, dst, rowLength, swapBytes;
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
            glGetIntegerv(GL_UNPACK_SWAP_BYTES, &swapBytes);
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_BLEND_SRC, &src);
            glGetIntegerv(GL_BLEND_DST, &dst);
            Require(alignment == 8 && rowLength == 31 && swapBytes == GL_TRUE && src == GL_SRC_ALPHA &&
                        dst == GL_ONE,
                    "GL blend/unpack state restoration");
            Check(d);
            if (floating) {
                if (d.floating_) {
                    Require(!glfwGetWindowAttrib(d.window_, GLFW_DECORATED), "floating window has no title bar");
                    ClickThemeButton(d);
                    Require(d.themeMenuOpen_, "left-click dropdown opens menu in floating mode");
                    GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);
                    Require(!d.themeMenuOpen_ && d.floating_ && !glfwGetWindowAttrib(d.window_, GLFW_DECORATED),
                            "Escape dismisses menu before restoring the floating frame");
                    int w, h;
                    auto pixels = Read(d, w, h);
                    Require(pixels[3] == 0 && pixels[(size_t(h / 2) * w + w / 2) * 4 + 3] == 255,
                            "floating window has transparent corners and an opaque body");
                    Require(pixels[(size_t(h / 2) * w + w - 10) * 4 + 3] == 0,
                            "space below the dropdown remains transparent");
                    std::vector<std::tuple<int, int, bool>> events;
                    d.SetKeyEventCallback([&](int bank, int pin, bool down) { events.emplace_back(bank, pin, down); });
                    int lw, lh;
                    glfwGetWindowSize(d.window_, &lw, &lh);
                    auto fit = d.PanelFit(lw, lh);
                    for (const auto &button : d.uiConfig_.buttons) {
                        UIPoint p{};
                        for (auto corner : button.region.polygon) {
                            p.x += corner.x / button.region.polygon.size();
                            p.y += corner.y / button.region.polygon.size();
                        }
                        events.clear();
                        GLFWDisplay::CursorPositionCallback(d.window_, fit.x + fit.scale * p.x, fit.y + fit.scale * p.y);
                        GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
                        Require(!d.dragging_, "instrument buttons do not drag the window");
                        GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
                        Require(events == std::vector<std::tuple<int, int, bool>>{{button.gpio.first, button.gpio.second, true},
                                                                                {button.gpio.first, button.gpio.second, false}},
                                "floating window preserves every button event");
                    }
                    const auto clickRegion = [&](const UIRegion &region) {
                        UIPoint p{};
                        for (auto corner : region.polygon) {
                            p.x += corner.x / region.polygon.size();
                            p.y += corner.y / region.polygon.size();
                        }
                        GLFWDisplay::CursorPositionCallback(d.window_, fit.x + fit.scale * p.x, fit.y + fit.scale * p.y);
                        GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
                        Require(!d.dragging_, "encoders and volume do not drag the window");
                        GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
                    };
                    for (const auto &encoder : d.uiConfig_.encoders) {
                        events.clear();
                        clickRegion(encoder.clockwise);
                        clickRegion(encoder.counterClockwise);
                        Require(events.size() == 4, "floating encoders retain both quadrature directions");
                    }
                    const auto volume = d.GetVolumeValue();
                    clickRegion(d.uiConfig_.volumeUp);
                    Require(d.GetVolumeValue() == volume + 5, "floating volume knob increases volume");
                    clickRegion(d.uiConfig_.volumeDown);
                    Require(d.GetVolumeValue() == volume, "floating volume knob decreases volume");
                    events.clear();
                    int wx, wy, movedX, movedY;
                    glfwGetWindowPos(d.window_, &wx, &wy);
                    const double x = fit.x + fit.scale * d.artWidth_ / 2, y = fit.y + fit.scale * 8;
                    GLFWDisplay::CursorPositionCallback(d.window_, x, y);
                    GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
                    Require(d.dragging_, "unbound casing starts a window drag");
                    GLFWDisplay::CursorPositionCallback(d.window_, x + 16, y + 12);
                    // End the synthetic gesture before pumping queued physical cursor events.
                    GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
                    glfwPollEvents();
                    glfwGetWindowPos(d.window_, &movedX, &movedY);
                    if (std::abs(movedX - wx - 16) > 1 || std::abs(movedY - wy - 12) > 1)
                        std::cerr << "native drag: " << wx << ',' << wy << " -> " << movedX << ',' << movedY << '\n';
                    Require(std::abs(movedX - wx - 16) <= 1 && std::abs(movedY - wy - 12) <= 1,
                            "casing drag moves the native window");
                    Require(!d.dragging_ && events.empty(), "window drag releases without instrument input");
                    GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_EQUAL, 0, GLFW_PRESS, GLFW_MOD_CONTROL);
                    glfwPollEvents();
                    int resizedWidth, resizedHeight;
                    glfwGetWindowSize(d.window_, &resizedWidth, &resizedHeight);
                    Require(resizedWidth > lw && std::abs(double(resizedWidth) / resizedHeight - d.CanvasWidth() / d.artHeight_) < .02,
                            "floating keyboard resize preserves the body and button-margin aspect");
                    GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_W, 0, GLFW_PRESS, GLFW_MOD_CONTROL);
                    Require(d.ShouldClose(), "floating close shortcut works");
                    glfwSetWindowShouldClose(d.window_, GLFW_FALSE);
                    GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);
                    d.SetKeyEventCallback({});
                    std::cout << "Floating alpha, all buttons, native drag, resize and recovery passed\n";
                } else
                    std::cout << "Floating unavailable; standard-window fallback checked\n";
                Require(!d.floating_ && glfwGetWindowAttrib(d.window_, GLFW_DECORATED),
                        "Escape or unavailable transparency retains a standard window");
                draw();
                int w, h;
                auto pixels = Read(d, w, h);
                Require(pixels[3] == 255, "standard-window recovery restores the opaque background");
            }
            // A magnified 2x2 RGB565 frame has a gray center with bilinear
            // sampling and a pure source color with nearest. Switch without
            // another frame upload to exercise draw-time filter selection.
            const auto defaultFilter = d.uiConfig_.screenFilter;
            Feed(d, 2, 2);
            d.uiConfig_.screenFilter = UIScreenFilter::Linear;
            draw();
            for (auto c : ScreenCenter(d))
                Require(std::abs(int(c) - 128) <= 12, "linear sampling interpolates screen pixels");
            const auto screenTexture = d.texture_;
            d.uiConfig_.screenFilter = UIScreenFilter::Nearest;
            draw();
            for (auto c : ScreenCenter(d))
                Require(c == 0 || c == 255, "nearest sampling preserves source colors");
            Require(d.texture_ == screenTexture, "filter change reuses display texture");
            d.uiConfig_.screenFilter = defaultFilter;
            Feed(d, 201, 400);
            draw();
            const auto texture = d.backgroundTexture_;
            glfwSetWindowSize(d.window_, 802, 500);
            glfwPollEvents();
            draw();
            std::this_thread::sleep_for(std::chrono::milliseconds(140));
            draw();
            Check(d);
            Require(d.backgroundTexture_ == texture, "resize reuses GL background texture");
            Feed(d, 203, 402);
            draw();
            Check(d);
            // An arbitrary projective screen quad must also keep corner colors.
            d.uiConfig_.screen = {UIPoint{284, 30}, {530, 40}, {515, 145}, {300, 135}};
            draw();
            Check(d);
            Require(glGetError() == GL_NO_ERROR, "GL resources and geometry changes");
            if (SvgBackgroundRenderer::Available()) {
                d.LoadPanel("tests/fixtures/svg/ui-alpha.json");
                draw();
                int w, h, lw, lh;
                auto rgba = Read(d, w, h);
                glfwGetWindowSize(d.window_, &lw, &lh);
                auto fit = UIFit::Into(20, 10, lw, lh);
                int x = int((fit.x + 15 * fit.scale) * w / lw), y = int((fit.y + 5 * fit.scale) * h / lh);
                auto pixel = rgba.data() + (size_t(y) * w + x) * 4;
                Require(std::abs(int(pixel[0]) - 15) <= 2 && std::abs(int(pixel[1]) - 143) <= 2 &&
                            std::abs(int(pixel[2]) - 15) <= 2,
                        "premultiplied alpha composites without dark/bright halos");
            }
            d.LoadPanel("gui/ui.json");
            Feed(d, 201, 400);
            draw();
            Check(d);
            Require(!d.svg_, "PNG remains usable after SVG");
            // Native window-sharing tools deliver correct cursor events while
            // a separate global glfwGetCursorPos query may remain stale.
            auto synth = std::find_if(d.uiConfig_.buttons.begin(), d.uiConfig_.buttons.end(),
                                      [](const auto &b) { return b.name == "synth"; });
            Require(synth != d.uiConfig_.buttons.end(), "Synth control exists");
            int inputWidth, inputHeight;
            glfwGetWindowSize(d.window_, &inputWidth, &inputHeight);
            auto inputFit = d.PanelFit(inputWidth, inputHeight);
            UIPoint center{};
            for (auto p : synth->region.polygon) {
                center.x += p.x / 4;
                center.y += p.y / 4;
            }
            std::vector<std::tuple<int, int, bool>> keys;
            d.SetKeyEventCallback([&](int bank, int pin, bool down) { keys.emplace_back(bank, pin, down); });
            GLFWDisplay::CursorPositionCallback(d.window_, inputFit.x + center.x * inputFit.scale,
                                                inputFit.y + center.y * inputFit.scale);
            GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
            GLFWDisplay::CursorPositionCallback(d.window_, -100, -100);
            GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
            Require(keys == std::vector<std::tuple<int, int, bool>>{{3, 2, true}, {3, 2, false}},
                    "mouse callbacks use event coordinates and release outside");
            keys.clear();
            GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, 0);
            GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE, 0);
            Require(!d.themeMenuOpen_ && keys.empty(), "PNG ignores theme menu without instrument events");
            GLFWDisplay::FocusCallback(d.window_, GLFW_FALSE);
            Require(!d.hasCursorPosition_, "focus loss invalidates stale cursor position");
            d.SetKeyEventCallback({});
            if (SvgBackgroundRenderer::Available()) {
                d.theme_.clear();
                d.LoadPanel("gui/ui-svg.json");
                Require(d.CanvasWidth() == 1268 && d.artWidth_ == 1204 && d.artHeight_ == 434,
                        "button margin adds 64 pixels without changing artwork dimensions");
                glfwSetWindowSize(d.window_, 1268, 434);
                draw();
                std::this_thread::sleep_for(std::chrono::milliseconds(140));
                draw();
                const auto layout = UILayout::Load("gui/ui-svg.json");
                const auto backgroundTexture = d.backgroundTexture_, liveTexture = d.texture_;
                auto *const sameWindow = d.window_;
                int lw, lh;
                glfwGetWindowSize(d.window_, &lw, &lh);
                const auto openMenu = [&] {
                    ClickThemeButton(d);
                    Require(d.themeMenuOpen_ && !d.dragging_, "dropdown opens without dragging the window");
                    Require(d.themeMenuPosition_.x + 220 * d.themeMenuScale_ <= lw &&
                                d.themeMenuPosition_.y + 244 * d.themeMenuScale_ <= lh,
                            "popup fits inside the window at its edges");
                    const auto fit = d.PanelFit(lw, lh);
                    Require(std::abs(d.themeMenuPosition_.x + 220 * d.themeMenuScale_ -
                                     (fit.x + (d.artWidth_ + 52) * fit.scale)) < 1 &&
                                d.themeMenuPosition_.y >= fit.y + 48 * fit.scale,
                            "menu opens below and aligns with the dropdown button");
                };
                const auto finishSwitch = [&] {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                    do {
                        Require(std::chrono::steady_clock::now() < deadline, "theme render completes");
                        Feed(d, 201, 400); // new framebuffer uploads continue during asynchronous theme rendering
                        draw();
                        Check(d);
                    } while (d.pendingTheme_ || d.themeRender_.valid());
                    Require(d.window_ == sameWindow && d.backgroundTexture_ == backgroundTexture &&
                                d.texture_ == liveTexture,
                            "theme switch reuses the native window and both GL textures");
                    Require(UIScreenMatrix(d.uiConfig_.screen) == UIScreenMatrix(layout.screen) &&
                                d.uiConfig_.screenFilter == layout.screenFilter &&
                                d.uiConfig_.buttons.size() == layout.buttons.size(),
                            "theme preserves screen geometry, filtering and control count");
                    for (size_t i = 0; i < layout.buttons.size(); ++i)
                        Require(d.uiConfig_.buttons[i].gpio == layout.buttons[i].gpio,
                                "theme switch preserves every GPIO binding");
                };
                keys.clear();
                d.SetKeyEventCallback([&](int bank, int pin, bool down) { keys.emplace_back(bank, pin, down); });
                GLFWDisplay::CursorPositionCallback(d.window_, 100, 100);
                GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS, 0);
                GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_RIGHT, GLFW_RELEASE, 0);
                Require(!d.themeMenuOpen_ && keys.empty(), "right-click no longer opens the moved selector");
                openMenu();
                ClickThemeButton(d);
                Require(!d.themeMenuOpen_ && !d.dragging_ && keys.empty(), "dropdown button toggles the menu closed");
                const char *presets[] = {"slate", "silver", "yellow", "green", "purple", "orange"};
                for (int i = 0; i < 6; ++i) {
                    openMenu();
                    Feed(d, 201, 400);
                    draw();
                    if (i == 1)
                        if (const char *capture = std::getenv("OP1_UI_MENU_CAPTURE")) {
                            int w, h;
                            const auto pixels = Read(d, w, h);
                            Require(stbi_write_png(capture, w, h, 4, pixels.data(), w * 4), "menu capture writes");
                        }
                    const double x = d.themeMenuPosition_.x + 75 * d.themeMenuScale_;
                    const double y = d.themeMenuPosition_.y + (53 + i * 34) * d.themeMenuScale_;
                    GLFWDisplay::CursorPositionCallback(d.window_, x, y);
                    Require(d.themeMenuHover_ == i, "each menu row maps to its visible palette");
                    GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
                    GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
                    Require(!d.themeMenuOpen_ && keys.empty(), "selection click never reaches OP-1 controls");
                    finishSwitch();
                    Require(d.theme_ == presets[i] && d.SelectedThemeRow() == i, "selected theme checkmark follows swap");
                }
                openMenu();
                GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_DOWN, 0, GLFW_PRESS, 0);
                Require(d.themeMenuHover_ == 0, "keyboard navigation wraps through presets");
                GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_ENTER, 0, GLFW_PRESS, 0);
                finishSwitch();
                Require(d.theme_ == "slate" && keys.empty(), "keyboard selection is isolated from firmware input");
                openMenu();
                GLFWDisplay::CursorPositionCallback(d.window_, 0, 0);
                GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
                GLFWDisplay::MouseButtonCallback(d.window_, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
                Require(!d.themeMenuOpen_ && !d.pendingTheme_ && keys.empty(), "outside click dismisses without input");
                GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_TAB, 0, GLFW_PRESS, 0);
                Require(d.themeButtonFocused_, "Tab focuses the dropdown");
                GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_ENTER, 0, GLFW_PRESS, 0);
                Require(d.themeMenuOpen_, "Enter opens the focused dropdown");
                GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);
                openMenu();
                GLFWDisplay::FocusCallback(d.window_, GLFW_FALSE);
                Require(!d.themeMenuOpen_, "focus loss dismisses the popup");
                d.RequestTheme("purple");
                draw();
                Require(d.themeRender_.valid(), "palette rendering runs as an asynchronous job");
                d.RequestTheme("green");
                finishSwitch();
                Require(d.theme_ == "green", "latest theme request wins over a stale render");
                d.RequestTheme("no-such-theme.json");
                finishSwitch();
                Require(d.theme_ == "green" && bool(d.svg_), "bad palette keeps the current theme");
                glfwSetWindowSize(d.window_, 317, 109);
                draw();
                ClickThemeButton(d);
                draw();
                Require(d.themeMenuPosition_.x >= 0 && d.themeMenuPosition_.y >= 0 &&
                            d.themeMenuPosition_.x + 220 * d.themeMenuScale_ <= 317 &&
                            d.themeMenuPosition_.y + 244 * d.themeMenuScale_ <= 109,
                        "dropdown stays reachable and fits at quarter-size");
                GLFWDisplay::KeyCallback(d.window_, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);
                d.SetKeyEventCallback({});
                draw();
                Check(d);
                std::cout << "Theme dropdown: outside-body button, anchoring, six live swaps, checkmarks, keyboard/outside/focus dismissal, input isolation, "
                             "texture reuse, live frames, latest-request and failure recovery passed\n";
            }
            std::cout
                << "GL RGB565, filtering, orientation, projective overlay, state, resize and texture reuse passed\n";
        } else if (argc > first + 1) {
            int w, h;
            auto pixels = Read(d, w, h);
            Require(stbi_write_png(argv[first + 1], w, h, 4, pixels.data(), w * 4), "capture write failed");
            std::cout << w << 'x' << h << " capture written\n";
        } else
            while (!d.ShouldClose()) {
                draw();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        return 0;
    }
};
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--probe-context") {
            // Capability probe only: renderer/assertion failures never skip.
            glfwSetErrorCallback(
                [](int code, const char *detail) { std::cerr << "GLFW " << code << ": " << detail << '\n'; });
            if (!glfwInit())
                return 77;
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            auto *window = glfwCreateWindow(640, 480, "OpenGL capability probe", nullptr, nullptr);
            if (!window) {
                glfwTerminate();
                return 77;
            }
            glfwDestroyWindow(window);
            glfwTerminate();
            return 0;
        }
        std::string theme;
        std::vector<char *> args{argv[0]};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--theme") {
                if (++i == argc || !*argv[i])
                    throw std::runtime_error("--theme requires a name or JSON path");
                theme = argv[i];
            } else if (arg.rfind("--theme=", 0) == 0) {
                theme = arg.substr(8);
                if (theme.empty())
                    throw std::runtime_error("--theme requires a name or JSON path");
            } else
                args.push_back(argv[i]);
        }
        return UIPreview::Run(static_cast<int>(args.size()), args.data(), theme);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
