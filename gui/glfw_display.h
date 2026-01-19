#pragma once

#include "peripheral/display.h"
#include "peripheral/keyboard.h"
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

struct ButtonConfig {
    int left;
    int top;
    int width;
    int height;
    int bank;
    int index;
    std::string name;
};

struct KeycapConfig {
    std::string name;
    int glfwKey; // GLFW key code
    int bank;
    int index;
};

struct UIConfig {
    std::string background;
    DisplayRect display;
    float scale = 1.0f;
    std::vector<ButtonConfig> buttons;
    std::vector<KeycapConfig> keycaps;
};

class GLFWDisplay : public Display, public Keyboard {
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
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    void HandleMouseButton(int button, int action, double x, double y);
    void HandleKeyboard(int key, int action);
    int MapKeyNameToGLFW(const std::string& keyName);

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
