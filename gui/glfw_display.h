#pragma once
#include "peripheral/display.h"
#include "peripheral/keyboard.h"
#include "svg_background_renderer.h"
#include "ui_layout.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class GLFWDisplay : public Display, public Keyboard {
  public:
    explicit GLFWDisplay(const std::string &configPath = "gui/ui.json", bool floating = false,
                         const std::string &theme = "");
    ~GLFWDisplay() override;
    void Initialize(int rows, int lines) override;
    void UpdateRowBuffer(int x, int y, const void *data, int length) override;
    void RenderFramebuffer(bool present = true);
    void PollEvents();
    bool ShouldClose() const;
    void SwapBuffers();
    void SetDisplayRotation(int degrees);
    uint8_t GetVolumeValue() const { return input_.Volume(); }

  private:
    friend class UIPreview; // firmware-free GL regression/capture harness
    void LoadPanel(const std::string &path);
    void UploadBackground(const uint8_t *data, int width, int height);
    void UpdateSvgCache(int logicalWidth, int logicalHeight, int fbWidth, int fbHeight);
    void Cleanup();
    static void CursorPositionCallback(GLFWwindow *, double x, double y);
    static void MouseButtonCallback(GLFWwindow *, int button, int action, int mods);
    static void KeyCallback(GLFWwindow *, int key, int scancode, int action, int mods);
    static void FocusCallback(GLFWwindow *, int focused);
    int MapKeyNameToGLFW(const std::string &name);
    void DrawBackground();
    void DrawScreen();
    bool IsControl(UIPoint point) const;
    void RestoreWindowFrame();
    void ResizeFloating(double factor);
    bool HasThemeButton() const;
    double CanvasWidth() const;
    // Shared by screen rendering, SVG raster density and logical mouse coordinates.
    UIFit PanelFit(int width, int height) const;
    bool ThemeButtonContains(double x, double y) const;
    void DrawThemeButton(int width, int height);
    void OpenThemeMenu();
    void CloseThemeMenu();
    void FitThemeMenu(int width, int height);
    int ThemeMenuRow(double x, double y) const;
    int SelectedThemeRow() const;
    void DrawThemeMenu(int width, int height, int fbWidth, int fbHeight);
    void RequestTheme(const std::string &theme);
    void UpdateThemeSwitch();
    struct ThemeRender {
        std::unique_ptr<SvgBackgroundRenderer> svg;
        SvgPixels pixels;
    };
    GLFWwindow *window_ = nullptr;
    GLuint texture_ = 0, backgroundTexture_ = 0;
    std::vector<uint16_t> framebuffer_;
    int width_ = 0, height_ = 0, textureWidth_ = 0, textureHeight_ = 0;
    int rotation_ = 90;
    bool dirty_ = false;
    std::mutex framebufferMutex_;
    UIPoint cursorPosition_;
    bool hasCursorPosition_ = false;
    bool floating_ = false, dragging_ = false;
    UIPoint dragOffset_;
    UILayout uiConfig_;
    UIPanelInput input_{uiConfig_};
    std::unique_ptr<SvgBackgroundRenderer> svg_;
    std::string theme_;
    std::future<ThemeRender> themeRender_;
    std::optional<std::string> pendingTheme_;
    std::string renderingTheme_;
    bool themeMenuOpen_ = false, suppressLeftRelease_ = false;
    bool themeButtonHovered_ = false, themeButtonFocused_ = false;
    UIPoint themeMenuPosition_;
    double themeMenuScale_ = 1;
    int themeMenuHover_ = -1, menuTextureWidth_ = 0, menuTextureHeight_ = 0;
    GLuint menuTexture_ = 0;
    std::unique_ptr<SvgBackgroundRenderer> menuSvg_;
    std::array<std::array<float, 3>, 6> themeSwatches_{};
    double artWidth_ = 0, artHeight_ = 0;
    int cachedWidth_ = 0, cachedHeight_ = 0, pendingWidth_ = 0, pendingHeight_ = 0;
    std::chrono::steady_clock::time_point resizeChanged_{};
};
