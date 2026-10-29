#include "glfw_display.h"
#include "svg_theme.h"
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace {
constexpr const char *themes[] = {"slate", "silver", "yellow", "green", "purple", "orange"};
constexpr double menuWidth = 220, menuHeight = 244, menuTop = 36, menuRowHeight = 34;
constexpr double themeRailWidth = 64, themeButtonGap = 12, themeButtonTop = 8, themeButtonSize = 40;
void MenuRect(double x, double y, double width, double height, double radius) {
    constexpr double pi = 3.14159265358979323846;
    glBegin(GL_TRIANGLE_FAN);
    glVertex2d(x + width / 2, y + height / 2);
    for (int c = 0; c < 4; ++c) {
        const double cx = x + (c == 0 || c == 3 ? width - radius : radius);
        const double cy = y + (c < 2 ? height - radius : radius);
        for (int step = 0; step <= 8; ++step) {
            const double angle = (c * .5 + step / 16.0) * pi;
            glVertex2d(cx + radius * std::cos(angle), cy + radius * std::sin(angle));
        }
    }
    glVertex2d(x + width, y + height - radius);
    glEnd();
}
} // namespace

GLFWDisplay::GLFWDisplay(const std::string &configPath, bool floating, const std::string &theme)
    : floating_(floating), theme_(theme) {
    if (!glfwInit())
        throw std::runtime_error("Failed to initialize GLFW");
    try {
        glfwDefaultWindowHints();
        if (floating_) {
            glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
            glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
            glfwWindowHint(GLFW_ALPHA_BITS, 8);
        }
        window_ = glfwCreateWindow(640, 480, "OP1 Emulator", nullptr, nullptr);
        if (!window_ && floating_) {
            std::cerr << "Floating window creation failed; using a standard window\n";
            floating_ = false;
            glfwDefaultWindowHints();
            window_ = glfwCreateWindow(640, 480, "OP1 Emulator", nullptr, nullptr);
        }
        if (!window_)
            throw std::runtime_error("Failed to create GLFW window");
        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);
        glfwSetWindowUserPointer(window_, this);
        glfwSetMouseButtonCallback(window_, MouseButtonCallback);
        glfwSetCursorPosCallback(window_, CursorPositionCallback);
        glfwSetKeyCallback(window_, KeyCallback);
        glfwSetWindowFocusCallback(window_, FocusCallback);
        input_.event = [this](int bank, int pin, bool pressed) {
            if (keyEventCallback)
                keyEventCallback(bank, pin, pressed);
        };
        try {
            LoadPanel(configPath);
        } catch (const std::exception &e) {
            if (configPath == "gui/ui.json")
                throw;
            std::cerr << "UI config failed: " << e.what() << "; falling back to gui/ui.json\n";
            LoadPanel("gui/ui.json");
        }
        glfwSetWindowSize(window_, std::max(1, int(CanvasWidth() * uiConfig_.scale)),
                          std::max(1, int(artHeight_ * uiConfig_.scale)));
        if (floating_) {
            if (!svg_ || !glfwGetWindowAttrib(window_, GLFW_TRANSPARENT_FRAMEBUFFER)) {
                std::cerr << "Floating UI needs SVG artwork and desktop transparency; using a standard window\n";
                RestoreWindowFrame();
            } else {
                glfwSetWindowAspectRatio(window_, std::max(1, int(std::lround(CanvasWidth()))),
                                         std::max(1, int(std::lround(artHeight_))));
            }
        }
    } catch (...) {
        Cleanup();
        throw;
    }
}
void GLFWDisplay::Cleanup() {
    if (themeRender_.valid())
        themeRender_.wait(); // worker owns no GLFW/GL state and captures no display pointer
    if (window_) {
        glfwMakeContextCurrent(window_);
        if (texture_)
            glDeleteTextures(1, &texture_);
        if (backgroundTexture_)
            glDeleteTextures(1, &backgroundTexture_);
        if (menuTexture_)
            glDeleteTextures(1, &menuTexture_);
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
}
GLFWDisplay::~GLFWDisplay() { Cleanup(); }
void GLFWDisplay::UploadBackground(const uint8_t *data, int width, int height) {
    GLint limit;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (width > limit || height > limit)
        throw std::runtime_error("Background exceeds GL texture size");
    glPushAttrib(GL_TEXTURE_BIT);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    if (!backgroundTexture_)
        glGenTextures(1, &backgroundTexture_);
    glBindTexture(GL_TEXTURE_2D, backgroundTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glPopClientAttrib();
    glPopAttrib();
}
void GLFWDisplay::LoadPanel(const std::string &path) {
    CloseThemeMenu();
    themeButtonHovered_ = themeButtonFocused_ = false;
    auto config = UILayout::Load(path);
    std::unique_ptr<SvgBackgroundRenderer> svg;
    if (config.svg) {
        if (!theme_.empty()) {
            try {
                if (config.themeBackground.empty())
                    throw std::runtime_error("Layout has no theme_background artwork");
                const auto theme = SvgTheme::Load(SvgTheme::Resolve(theme_));
                svg = std::make_unique<SvgBackgroundRenderer>(config.themeBackground, theme.Stylesheet());
            } catch (const std::exception &e) {
                std::cerr << "SVG theme failed: " << e.what() << "; using original SVG colors\n";
                theme_.clear();
            }
        }
        if (!svg)
            svg = std::make_unique<SvgBackgroundRenderer>(config.background);
        auto pixels = svg->Render(uint32_t(std::ceil(svg->Width())), uint32_t(std::ceil(svg->Height())));
        UploadBackground(pixels.rgba.data(), pixels.width, pixels.height);
        artWidth_ = svg->Width();
        artHeight_ = svg->Height();
        cachedWidth_ = pixels.width;
        cachedHeight_ = pixels.height;
    } else {
        if (!theme_.empty())
            std::cerr << "SVG themes are unavailable for the PNG panel; using original PNG colors\n";
        int width, height, channels;
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> data(
            stbi_load(config.background.c_str(), &width, &height, &channels, 4), stbi_image_free);
        if (!data)
            throw std::runtime_error("Cannot load PNG: " + config.background);
        UploadBackground(data.get(), width, height);
        artWidth_ = width;
        artHeight_ = height;
    }
    uiConfig_ = std::move(config);
    svg_ = std::move(svg);
}
void GLFWDisplay::Initialize(int rows, int lines) {
    if (rows <= 0 || lines <= 0 || rows > 8192 || lines > 8192)
        throw std::runtime_error("Invalid framebuffer size");
    std::lock_guard<std::mutex> lock(framebufferMutex_);
    if (rows == width_ && lines == height_)
        return;
    width_ = rows;
    height_ = lines;
    framebuffer_.assign(size_t(rows) * lines, 0);
    dirty_ = true;
}
void GLFWDisplay::UpdateRowBuffer(int x, int y, const void *data, int length) {
    std::lock_guard<std::mutex> lock(framebufferMutex_);
    if (!data || length <= 0 || y < 0 || y >= height_ || x < 0 || x >= width_)
        return;
    const int pixels = std::min(length / int(sizeof(uint16_t)), width_ - x);
    if (pixels > 0) {
        std::memcpy(framebuffer_.data() + size_t(y) * width_ + x, data, size_t(pixels) * 2);
        dirty_ = true;
    }
}
void GLFWDisplay::UpdateSvgCache(int lw, int lh, int fw, int fh) {
    if (!svg_)
        return;
    const auto fit = PanelFit(lw, lh);
    double w = artWidth_ * fit.scale * fw / lw, h = artHeight_ * fit.scale * fh / lh;
    GLint maxTexture;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
    const double limit = std::min(8192, maxTexture);
    double reduction = std::min({1.0, limit / w, limit / h, std::sqrt((16.0 * 1024 * 1024) / (w * h))});
    const int targetWidth = std::max(1, int(std::floor(w * reduction))),
              targetHeight = std::max(1, int(std::floor(h * reduction)));
    if (cachedWidth_ == targetWidth && cachedHeight_ == targetHeight)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (targetWidth != pendingWidth_ || targetHeight != pendingHeight_) {
        pendingWidth_ = targetWidth;
        pendingHeight_ = targetHeight;
        resizeChanged_ = now;
        return;
    }
    if (now - resizeChanged_ < std::chrono::milliseconds(120))
        return;
    try {
        auto pixels = svg_->Render(targetWidth, targetHeight);
        UploadBackground(pixels.rgba.data(), targetWidth, targetHeight);
        cachedWidth_ = targetWidth;
        cachedHeight_ = targetHeight;
    } catch (const std::exception &e) {
        std::cerr << "SVG resize failed: " << e.what() << "; keeping cached texture\n";
        // Avoid retrying expensive filters every frame after a failed allocation.
        cachedWidth_ = targetWidth;
        cachedHeight_ = targetHeight;
    }
}
void GLFWDisplay::DrawBackground() {
    glBlendFunc(svg_ ? GL_ONE : GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, backgroundTexture_);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex2d(0, 0);
    glTexCoord2f(1, 0);
    glVertex2d(artWidth_, 0);
    glTexCoord2f(1, 1);
    glVertex2d(artWidth_, artHeight_);
    glTexCoord2f(0, 1);
    glVertex2d(0, artHeight_);
    glEnd();
}
void GLFWDisplay::DrawScreen() {
    // Map the portrait DMA texture to TL/TR/BR/BL in landscape. Homogeneous
    // vertices give perspective-correct interpolation for non-affine quads.
    const auto matrix = UIScreenMatrix(uiConfig_.screen);
    glPushMatrix();
    glMultMatrixd(matrix.data());
    glDisable(GL_BLEND); // firmware RGB565 is opaque, independent of SVG alpha
    if (texture_) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture_);
        const GLint filter = uiConfig_.screenFilter == UIScreenFilter::Linear ? GL_LINEAR : GL_NEAREST;
        // Apply at draw time: a panel switch must work even without a new
        // firmware frame. PNG/custom layouts keep nearest unless opted in.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    } else {
        glDisable(GL_TEXTURE_2D);
        glColor4f(0, 0, 0, 1);
    }
    constexpr double pi = 3.14159265358979323846;
    const double c = std::cos(rotation_ * pi / 180), s = std::sin(rotation_ * pi / 180);
    const std::array<UIPoint, 4> corners = {UIPoint{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    glBegin(GL_QUADS);
    for (auto p : corners) {
        const double x = p.x - .5, y = p.y - .5;
        glTexCoord2d(c * x + s * y + .5, -s * x + c * y + .5);
        glVertex2d(p.x, p.y);
    }
    glEnd();
    glColor4f(1, 1, 1, 1);
    glPopMatrix();
}
void GLFWDisplay::RenderFramebuffer(bool present) {
    if (!window_)
        return;
    glfwMakeContextCurrent(window_);
    UpdateThemeSwitch();
    int fw, fh, lw, lh;
    glfwGetFramebufferSize(window_, &fw, &fh);
    glfwGetWindowSize(window_, &lw, &lh);
    if (fw <= 0 || fh <= 0 || lw <= 0 || lh <= 0)
        return; // minimized
    UpdateSvgCache(lw, lh, fw, fh);
    // The compatibility context is owned by this frontend. Restore its GL
    // state so premultiplied SVG state never leaks into PNG or display upload.
    GLint matrixMode;
    glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE); // RGB565 rows may be an odd width
    {
        std::lock_guard<std::mutex> lock(framebufferMutex_);
        if (dirty_) {
            if (!texture_)
                glGenTextures(1, &texture_);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (textureWidth_ != width_ || textureHeight_ != height_) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width_, height_, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                             framebuffer_.data());
                textureWidth_ = width_;
                textureHeight_ = height_;
            } else
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                                framebuffer_.data());
            dirty_ = false;
        }
    }
    glViewport(0, 0, fw, fh);
    if (floating_)
        glClearColor(0, 0, 0, 0);
    else
        glClearColor(.12f, .12f, .12f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, lw, lh, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    const auto fit = PanelFit(lw, lh);
    glTranslated(fit.x, fit.y, 0);
    glScaled(fit.scale, fit.scale, 1);
    glColor4f(1, 1, 1, 1);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    DrawBackground();
    DrawScreen();
    glLoadIdentity();
    DrawThemeButton(lw, lh);
    DrawThemeMenu(lw, lh, fw, fh);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(matrixMode);
    glPopClientAttrib();
    glPopAttrib();
    if (present)
        glfwSwapBuffers(window_);
}
void GLFWDisplay::PollEvents() {
    glfwPollEvents();
    RenderFramebuffer();
}
bool GLFWDisplay::ShouldClose() const { return window_ && glfwWindowShouldClose(window_); }
void GLFWDisplay::SwapBuffers() {
    if (window_)
        glfwSwapBuffers(window_);
}
void GLFWDisplay::SetDisplayRotation(int degrees) { rotation_ = (degrees % 360 + 360) % 360; }
bool GLFWDisplay::IsControl(UIPoint point) const {
    for (const auto &b : uiConfig_.buttons)
        if (b.region.Contains(point))
            return true;
    for (const auto &e : uiConfig_.encoders)
        if (e.clockwise.Contains(point) || e.counterClockwise.Contains(point))
            return true;
    return uiConfig_.volumeUp.Contains(point) || uiConfig_.volumeDown.Contains(point);
}
void GLFWDisplay::RestoreWindowFrame() {
    floating_ = dragging_ = false;
    glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_TRUE);
    glfwSetWindowAspectRatio(window_, GLFW_DONT_CARE, GLFW_DONT_CARE);
}
void GLFWDisplay::ResizeFloating(double factor) {
    int width, height;
    glfwGetWindowSize(window_, &width, &height);
    const int base = std::max(4, int(std::lround(CanvasWidth() * uiConfig_.scale)));
    width = std::clamp(int(std::lround(width * factor)), base / 4, base * 3);
    height = std::max(1, int(std::lround(width * artHeight_ / CanvasWidth())));
    glfwSetWindowSize(window_, width, height);
}
bool GLFWDisplay::HasThemeButton() const {
    return svg_ && !uiConfig_.themeBackground.empty();
}
double GLFWDisplay::CanvasWidth() const {
    return artWidth_ + (HasThemeButton() ? themeRailWidth : 0);
}
UIFit GLFWDisplay::PanelFit(int width, int height) const {
    return UIFit::Into(CanvasWidth(), artHeight_, width, height);
}
bool GLFWDisplay::ThemeButtonContains(double x, double y) const {
    if (!HasThemeButton())
        return false;
    int width, height;
    glfwGetWindowSize(window_, &width, &height);
    if (width <= 0 || height <= 0)
        return false;
    const auto point = PanelFit(width, height).ToArt({x, y});
    return point.x >= artWidth_ + themeButtonGap && point.x < artWidth_ + themeButtonGap + themeButtonSize &&
           point.y >= themeButtonTop && point.y < themeButtonTop + themeButtonSize;
}
void GLFWDisplay::DrawThemeButton(int width, int height) {
    if (!HasThemeButton())
        return;
    const auto fit = PanelFit(width, height);
    glPushMatrix();
    glTranslated(fit.x + (artWidth_ + themeButtonGap) * fit.scale, fit.y + themeButtonTop * fit.scale, 0);
    glScaled(fit.scale, fit.scale, 1);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (themeButtonFocused_)
        glColor4f(.46f, .65f, 1, 1);
    else
        glColor4f(.36f, .38f, .43f, 1);
    MenuRect(0, 0, themeButtonSize, themeButtonSize, 9);
    if (themeMenuOpen_ || themeButtonHovered_)
        glColor4f(.25f, .28f, .34f, 1);
    else
        glColor4f(.14f, .15f, .18f, 1);
    MenuRect(1, 1, themeButtonSize - 2, themeButtonSize - 2, 8);
    glColor4f(.93f, .94f, .97f, 1);
    if (themeMenuOpen_) {
        glTranslated(0, themeButtonSize, 0);
        glScaled(1, -1, 1);
    }
    // Filled chevron keeps a consistent shape at fractional and Retina scales.
    glBegin(GL_QUADS);
    glVertex2d(12, 17);
    glVertex2d(14, 15);
    glVertex2d(20, 21);
    glVertex2d(20, 25);
    glVertex2d(20, 21);
    glVertex2d(26, 15);
    glVertex2d(28, 17);
    glVertex2d(20, 25);
    glEnd();
    glPopMatrix();
}
int GLFWDisplay::SelectedThemeRow() const {
    if (theme_.empty())
        return 1; // unmodified supplied artwork is Silver
    for (int i = 0; i < 6; ++i)
        if (std::filesystem::path(SvgTheme::Resolve(theme_)).lexically_normal() ==
            std::filesystem::path(SvgTheme::Resolve(themes[i])).lexically_normal())
            return i;
    return -1; // a custom palette has no preset checkmark
}
void GLFWDisplay::FitThemeMenu(int width, int height) {
    themeMenuScale_ = std::max(.01, std::min({1.0, (width - 8.0) / menuWidth, (height - 8.0) / menuHeight}));
    const auto fit = PanelFit(width, height);
    themeMenuPosition_ = {fit.x + (artWidth_ + themeButtonGap + themeButtonSize) * fit.scale - menuWidth * themeMenuScale_,
                          fit.y + (themeButtonTop + themeButtonSize + 8) * fit.scale};
    themeMenuPosition_.x = std::clamp(themeMenuPosition_.x, 4.0, std::max(4.0, width - menuWidth * themeMenuScale_ - 4));
    themeMenuPosition_.y = std::clamp(themeMenuPosition_.y, 4.0, std::max(4.0, height - menuHeight * themeMenuScale_ - 4));
}
int GLFWDisplay::ThemeMenuRow(double x, double y) const {
    x = (x - themeMenuPosition_.x) / themeMenuScale_;
    y = (y - themeMenuPosition_.y) / themeMenuScale_;
    if (x < 6 || x >= menuWidth - 6 || y < menuTop || y >= menuTop + 6 * menuRowHeight)
        return -1;
    return int((y - menuTop) / menuRowHeight);
}
void GLFWDisplay::OpenThemeMenu() {
    if (!HasThemeButton())
        return;
    try {
        if (!menuSvg_)
            menuSvg_ = std::make_unique<SvgBackgroundRenderer>("gui/theme-menu.svg");
        for (int i = 0; i < 6; ++i) {
            const auto palette = SvgTheme::Load(SvgTheme::Resolve(themes[i]));
            const auto color = std::stoul(palette.colors.at("body").substr(1), nullptr, 16);
            themeSwatches_[i] = {float((color >> 16) & 255) / 255, float((color >> 8) & 255) / 255,
                                 float(color & 255) / 255};
        }
        int width, height;
        glfwGetWindowSize(window_, &width, &height);
        FitThemeMenu(width, height);
        themeMenuHover_ = SelectedThemeRow();
        themeMenuOpen_ = true;
        dragging_ = false;
        input_.ReleaseAll(); // the popup owns input until dismissed; no held notes get stuck
    } catch (const std::exception &e) {
        std::cerr << "Cannot open theme menu: " << e.what() << '\n';
        CloseThemeMenu();
    }
}
void GLFWDisplay::CloseThemeMenu() {
    themeMenuOpen_ = false;
    themeMenuHover_ = -1;
}
void GLFWDisplay::RequestTheme(const std::string &theme) {
    if (svg_ && !uiConfig_.themeBackground.empty())
        pendingTheme_ = theme;
}
void GLFWDisplay::UpdateThemeSwitch() {
    if (themeRender_.valid() && themeRender_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto ready = themeRender_.get();
            if (pendingTheme_ && *pendingTheme_ == renderingTheme_) {
                UploadBackground(ready.pixels.rgba.data(), ready.pixels.width, ready.pixels.height);
                svg_ = std::move(ready.svg);
                cachedWidth_ = ready.pixels.width;
                cachedHeight_ = ready.pixels.height;
                theme_ = renderingTheme_;
                pendingTheme_.reset();
                std::clog << "SVG theme switched to " << theme_ << '\n';
            }
        } catch (const std::exception &e) {
            std::cerr << "Theme switch failed: " << e.what() << "; keeping current theme\n";
            if (pendingTheme_ && *pendingTheme_ == renderingTheme_)
                pendingTheme_.reset();
        }
    }
    if (pendingTheme_ && !themeRender_.valid()) {
        if (*pendingTheme_ == theme_ || (theme_.empty() && *pendingTheme_ == "silver")) {
            pendingTheme_.reset();
            return;
        }
        renderingTheme_ = *pendingTheme_;
        const auto path = uiConfig_.themeBackground, theme = renderingTheme_;
        const auto width = std::max(1, cachedWidth_), height = std::max(1, cachedHeight_);
        const auto artWidth = artWidth_, artHeight = artHeight_;
        try {
            themeRender_ = std::async(std::launch::async, [path, theme, width, height, artWidth, artHeight] {
                auto svg = std::make_unique<SvgBackgroundRenderer>(path,
                    SvgTheme::Load(SvgTheme::Resolve(theme)).Stylesheet());
                if (svg->Width() != artWidth || svg->Height() != artHeight)
                    throw std::runtime_error("Theme artwork dimensions differ from the active layout");
                auto pixels = svg->Render(width, height);
                return ThemeRender{std::move(svg), std::move(pixels)};
            });
        } catch (const std::exception &e) {
            std::cerr << "Cannot start theme rendering: " << e.what() << "; keeping current theme\n";
            pendingTheme_.reset();
        }
    }
}
void GLFWDisplay::DrawThemeMenu(int width, int height, int fbWidth, int fbHeight) {
    if (!themeMenuOpen_)
        return;
    FitThemeMenu(width, height);
    const int tw = std::max(1, int(std::ceil(menuWidth * themeMenuScale_ * fbWidth / width))),
              th = std::max(1, int(std::ceil(menuHeight * themeMenuScale_ * fbHeight / height)));
    if (tw != menuTextureWidth_ || th != menuTextureHeight_) {
        auto pixels = menuSvg_->Render(tw, th);
        if (!menuTexture_)
            glGenTextures(1, &menuTexture_);
        glBindTexture(GL_TEXTURE_2D, menuTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.rgba.data());
        menuTextureWidth_ = tw;
        menuTextureHeight_ = th;
    }
    glPushMatrix();
    glTranslated(themeMenuPosition_.x, themeMenuPosition_.y, 0);
    glScaled(themeMenuScale_, themeMenuScale_, 1);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0, 0, 0, .3f);
    MenuRect(2, 3, menuWidth, menuHeight, 9);
    glColor4f(.14f, .15f, .18f, 1);
    MenuRect(0, 0, menuWidth, menuHeight, 9);
    for (int i = 0; i < 6; ++i) {
        const auto y = menuTop + i * menuRowHeight;
        if (themeMenuHover_ == i) {
            glColor4f(.25f, .28f, .34f, 1);
            MenuRect(6, y, menuWidth - 12, menuRowHeight - 2, 5);
        }
        glColor4f(themeSwatches_[i][0], themeSwatches_[i][1], themeSwatches_[i][2], 1);
        MenuRect(30, y + 9, 14, 14, 3);
        if (SelectedThemeRow() == i) {
            glColor4f(.95f, .95f, .97f, 1);
            glLineWidth(1.5f);
            glBegin(GL_LINE_STRIP);
            glVertex2d(13, y + 16);
            glVertex2d(17, y + 20);
            glVertex2d(23, y + 12);
            glEnd();
        }
    }
    glColor4f(1, 1, 1, 1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, menuTexture_);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2d(0, 0);
    glTexCoord2f(1, 0); glVertex2d(menuWidth, 0);
    glTexCoord2f(1, 1); glVertex2d(menuWidth, menuHeight);
    glTexCoord2f(0, 1); glVertex2d(0, menuHeight);
    glEnd();
    glPopMatrix();
}
void GLFWDisplay::CursorPositionCallback(GLFWwindow *w, double x, double y) {
    auto *self = static_cast<GLFWDisplay *>(glfwGetWindowUserPointer(w));
    self->cursorPosition_ = {x, y};
    self->hasCursorPosition_ = true;
    self->themeButtonHovered_ = self->ThemeButtonContains(x, y);
    if (self->themeMenuOpen_) {
        self->themeMenuHover_ = self->ThemeMenuRow(x, y);
        return;
    }
    if (self->dragging_) {
        int wx, wy;
        glfwGetWindowPos(w, &wx, &wy);
        glfwSetWindowPos(w, wx + int(std::lround(x - self->dragOffset_.x)),
                         wy + int(std::lround(y - self->dragOffset_.y)));
    }
}
void GLFWDisplay::MouseButtonCallback(GLFWwindow *w, int button, int action, int) {
    if (button != GLFW_MOUSE_BUTTON_LEFT || (action != GLFW_PRESS && action != GLFW_RELEASE))
        return;
    auto *self = static_cast<GLFWDisplay *>(glfwGetWindowUserPointer(w));
    double x, y;
    int width, height;
    // Native/remote event coordinates can differ from a global cursor query
    // (notably macOS window sharing). Prefer the latest GLFW cursor event.
    if (self->hasCursorPosition_) {
        x = self->cursorPosition_.x;
        y = self->cursorPosition_.y;
    } else {
        glfwGetCursorPos(w, &x, &y);
    }
    glfwGetWindowSize(w, &width, &height);
    if (self->suppressLeftRelease_ && action == GLFW_RELEASE) {
        self->suppressLeftRelease_ = false;
        return;
    }
    if (action == GLFW_PRESS && self->ThemeButtonContains(x, y)) {
        self->themeButtonFocused_ = true;
        if (self->themeMenuOpen_)
            self->CloseThemeMenu();
        else
            self->OpenThemeMenu();
        self->suppressLeftRelease_ = true;
        self->dragging_ = false;
        return;
    }
    if (self->themeMenuOpen_) {
        if (action == GLFW_PRESS) {
            const int row = self->ThemeMenuRow(x, y);
            if (row >= 0)
                self->RequestTheme(themes[row]);
            else
                self->themeButtonFocused_ = false;
            self->CloseThemeMenu();
            self->suppressLeftRelease_ = true;
        }
        return;
    }
    if (action == GLFW_RELEASE) {
        self->dragging_ = false;
        self->input_.Mouse(false, {});
    } else if (width > 0 && height > 0) {
        self->themeButtonFocused_ = false;
        const auto point = self->PanelFit(width, height).ToArt({x, y});
        if (point.x < 0 || point.y < 0 || point.x > self->artWidth_ || point.y > self->artHeight_)
            return; // the transparent button margin is not an instrument control or drag region
        if (self->floating_ && point.x >= 0 && point.y >= 0 && point.x <= self->artWidth_ &&
            point.y <= self->artHeight_ && !self->IsControl(point)) {
            self->dragging_ = true;
            self->dragOffset_ = {x, y};
        } else
            self->input_.Mouse(true, point);
    }
}
void GLFWDisplay::KeyCallback(GLFWwindow *w, int key, int, int action, int mods) {
    if (action != GLFW_PRESS && action != GLFW_RELEASE)
        return;
    auto *self = static_cast<GLFWDisplay *>(glfwGetWindowUserPointer(w));
    if (self->themeMenuOpen_) {
        if (action == GLFW_PRESS) {
            if (key == GLFW_KEY_ESCAPE)
                self->CloseThemeMenu();
            else if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) {
                if (self->themeMenuHover_ < 0)
                    self->themeMenuHover_ = key == GLFW_KEY_UP ? 5 : 0;
                else
                    self->themeMenuHover_ = (self->themeMenuHover_ + (key == GLFW_KEY_UP ? 5 : 1)) % 6;
            } else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_SPACE) {
                if (self->themeMenuHover_ >= 0)
                    self->RequestTheme(themes[self->themeMenuHover_]);
                self->CloseThemeMenu();
            }
        }
        return;
    }
    if (self->HasThemeButton()) {
        if (key == GLFW_KEY_TAB) {
            if (action == GLFW_PRESS) {
                self->themeButtonFocused_ = !self->themeButtonFocused_;
                self->input_.ReleaseAll();
            }
            return;
        }
        if (self->themeButtonFocused_ && action == GLFW_PRESS &&
            (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_SPACE)) {
            self->OpenThemeMenu();
            return;
        }
    }
    if (self->floating_ && action == GLFW_PRESS) {
        if (key == GLFW_KEY_ESCAPE) {
            self->RestoreWindowFrame();
            return;
        }
        if (mods & (GLFW_MOD_SUPER | GLFW_MOD_CONTROL)) {
            if (key == GLFW_KEY_W) {
                glfwSetWindowShouldClose(w, GLFW_TRUE);
                return;
            }
            if (key == GLFW_KEY_M) {
                glfwIconifyWindow(w);
                return;
            }
            if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) {
                self->ResizeFloating(1.1);
                return;
            }
            if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) {
                self->ResizeFloating(1 / 1.1);
                return;
            }
        }
    }
    for (auto &k : self->uiConfig_.keys)
        if (key == self->MapKeyNameToGLFW(k.name)) {
            self->input_.Key(key, k.gpio, action == GLFW_PRESS);
            break;
        }
}
void GLFWDisplay::FocusCallback(GLFWwindow *w, int focused) {
    if (!focused) {
        auto *self = static_cast<GLFWDisplay *>(glfwGetWindowUserPointer(w));
        self->input_.ReleaseAll();
        self->CloseThemeMenu();
        self->suppressLeftRelease_ = false;
        self->themeButtonHovered_ = self->themeButtonFocused_ = false;
        self->hasCursorPosition_ = false;
        self->dragging_ = false;
    }
}
int GLFWDisplay::MapKeyNameToGLFW(const std::string &keyName) {
    // Map common key names to GLFW key codes
    if (keyName == "shift")
        return GLFW_KEY_LEFT_SHIFT;
    if (keyName == "ctrl" || keyName == "control")
        return GLFW_KEY_LEFT_CONTROL;
    if (keyName == "alt")
        return GLFW_KEY_LEFT_ALT;
    if (keyName == "space")
        return GLFW_KEY_SPACE;
    if (keyName == "enter" || keyName == "return")
        return GLFW_KEY_ENTER;
    if (keyName == "tab")
        return GLFW_KEY_TAB;
    if (keyName == "escape" || keyName == "esc")
        return GLFW_KEY_ESCAPE;
    if (keyName == "backspace")
        return GLFW_KEY_BACKSPACE;

    // Arrow keys
    if (keyName == "up")
        return GLFW_KEY_UP;
    if (keyName == "down")
        return GLFW_KEY_DOWN;
    if (keyName == "left")
        return GLFW_KEY_LEFT;
    if (keyName == "right")
        return GLFW_KEY_RIGHT;

    // Single character keys (a-z, 0-9)
    if (keyName.length() == 1) {
        char c = keyName[0];
        if (c >= 'a' && c <= 'z')
            return GLFW_KEY_A + (c - 'a');
        if (c >= 'A' && c <= 'Z')
            return GLFW_KEY_A + (c - 'A');
        if (c >= '0' && c <= '9')
            return GLFW_KEY_0 + (c - '0');
    }

    // Default: return the key name as-is if it's already a valid GLFW key code
    return GLFW_KEY_UNKNOWN;
}
