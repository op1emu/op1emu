#pragma once

#include "peripheral/display.h"
#include <GLFW/glfw3.h>
#include <vector>
#include <cstdint>
#include <string>
#include <mutex>

struct DisplayRect {
    int left;
    int top;
    int width;
    int height;
};

struct UIConfig {
    std::string background;
    DisplayRect display;
    float scale = 1.0f;
};

class GLFWDisplay : public Display {
public:
    GLFWDisplay();
    ~GLFWDisplay() override;

    // Display interface
    void Initialize(int rows, int lines) override;
    void UpdateRowBuffer(int x, int y, const void* data, int length) override;
    void SetOnFrameStartCallback(const std::function<void(Display&)>& callback) override {
        onFrameStartCallback_ = callback;
    }
    void RenderFramebuffer();
    void PollEvents();
    bool ShouldClose() const;
    void SwapBuffers();
    void SetDisplayRotation(int degrees);

private:
    void CreateTexture();
    void LoadUIConfig(const std::string& path);
    GLuint LoadBackgroundTexture(const std::string& path, int& width, int& height);
    void DrawTexturedQuad(GLuint texture, float x, float y, float w, float h, float texW = 1.0f, float texH = 1.0f);

    GLFWwindow* window_ = nullptr;
    GLuint texture_ = 0;
    GLuint backgroundTexture_ = 0;
    std::vector<uint16_t> framebuffer_;
    int width_ = 0;
    int height_ = 0;
    int scale_ = 2;  // Scale factor for display
    int rotation_ = 90;  // Rotation in degrees
    bool dirty_ = false;
    std::mutex framebufferMutex_;  // Protect framebuffer access
    std::function<void(Display&)> onFrameStartCallback_;

    UIConfig uiConfig_;
    int windowWidth_ = 0;
    int windowHeight_ = 0;
};
