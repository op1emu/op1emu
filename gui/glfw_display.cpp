#include "glfw_display.h"
#include <GL/gl.h>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

GLFWDisplay::GLFWDisplay() {
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }
    LoadUIConfig("gui/ui.json");

    window_ = glfwCreateWindow(640, 480, "OP1 Emulator", nullptr, nullptr);
    if (!window_) {
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    // Set up mouse button callback
    glfwSetWindowUserPointer(window_, this);
    glfwSetMouseButtonCallback(window_, MouseButtonCallback);
    glfwSetKeyCallback(window_, KeyCallback);

    backgroundTexture_ = LoadBackgroundTexture(uiConfig_.background, windowWidth_, windowHeight_);
    windowWidth_ = static_cast<int>(windowWidth_ * uiConfig_.scale);
    windowHeight_ = static_cast<int>(windowHeight_ * uiConfig_.scale);
    glfwSetWindowSize(window_, windowWidth_, windowHeight_);
}

GLFWDisplay::~GLFWDisplay() {
    if (texture_) {
        glDeleteTextures(1, &texture_);
    }
    if (window_) {
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

void GLFWDisplay::Initialize(int rows, int lines) {
    width_ = rows;
    height_ = lines;

    framebuffer_.resize(width_ * height_, 0);
}

void GLFWDisplay::LoadUIConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open UI config: " + path);
    }

    nlohmann::json j;
    file >> j;

    uiConfig_.background = j["background"];
    uiConfig_.scale = j["scale"];
    uiConfig_.display.left = int(int(j["display"]["left"]) * uiConfig_.scale);
    uiConfig_.display.top = int(int(j["display"]["top"]) * uiConfig_.scale);
    uiConfig_.display.width = int(int(j["display"]["width"]) * uiConfig_.scale);
    uiConfig_.display.height = int(int(j["display"]["height"]) * uiConfig_.scale);

    // Parse buttons
    if (j.contains("buttons")) {
        for (auto& [name, btn] : j["buttons"].items()) {
            ButtonConfig button;
            button.name = name;
            auto&& pos = btn["position"];
            button.left = int(int(pos[0]) * uiConfig_.scale);
            button.top = int(int(pos[1]) * uiConfig_.scale);
            button.width = int(int(pos[2]) * uiConfig_.scale);
            button.height = int(int(pos[3]) * uiConfig_.scale);
            auto&& gpio = btn["gpio"];
            button.bank = gpio[0];
            button.index = gpio[1];
            uiConfig_.buttons.push_back(button);
        }
    }

    // Parse keycaps
    if (j.contains("keycaps")) {
        for (auto& [name, key] : j["keycaps"].items()) {
            KeycapConfig keycap;
            keycap.name = name;
            keycap.bank = key[0];
            keycap.index = key[1];
            keycap.glfwKey = MapKeyNameToGLFW(name);
            uiConfig_.keycaps.push_back(keycap);
        }
    }
}

GLuint GLFWDisplay::LoadBackgroundTexture(const std::string& path, int& width, int& height) {
    int channels;
    std::string fullPath = "gui/" + path;
    unsigned char* data = stbi_load(fullPath.c_str(), &width, &height, &channels, 4);
    if (!data) {
        throw std::runtime_error("Failed to load background image: " + fullPath);
    }

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    stbi_image_free(data);
    return texture;
}

void GLFWDisplay::CreateTexture() {
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width_, height_, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, framebuffer_.data());
}

void GLFWDisplay::UpdateRowBuffer(int x, int y, const void* data, int length) {
    if (y < 0 || y >= height_ || x < 0) return;

    int pixelsToCopy = length / sizeof(uint16_t);
    if (x + pixelsToCopy > width_) {
        pixelsToCopy = width_ - x;
    }

    if (pixelsToCopy > 0) {
        std::lock_guard<std::mutex> lock(framebufferMutex_);
        std::memcpy(&framebuffer_[y * width_ + x], data, pixelsToCopy * sizeof(uint16_t));
        dirty_ = true;
    }
}

void GLFWDisplay::DrawTexturedQuad(GLuint texture, float x, float y, float w, float h, float texW, float texH) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);    glVertex2f(x, y);
    glTexCoord2f(texW, 0); glVertex2f(x + w, y);
    glTexCoord2f(texW, texH); glVertex2f(x + w, y + h);
    glTexCoord2f(0, texH); glVertex2f(x, y + h);
    glEnd();
}

void GLFWDisplay::RenderFramebuffer() {
    if (!window_) return;

    glfwMakeContextCurrent(window_);

    if (dirty_) {
        std::lock_guard<std::mutex> lock(framebufferMutex_);
        if (texture_ == 0) {
            CreateTexture();
        }
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_,
                        GL_RGB, GL_UNSIGNED_SHORT_5_6_5, framebuffer_.data());
        dirty_ = false;
    }

    int fbWidth, fbHeight;
    glfwGetFramebufferSize(window_, &fbWidth, &fbHeight);
    glViewport(0, 0, fbWidth, fbHeight);

    glClear(GL_COLOR_BUFFER_BIT);

    // Set up orthographic projection for pixel-perfect rendering
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, windowWidth_, windowHeight_, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Draw background
    if (backgroundTexture_) {
        DrawTexturedQuad(backgroundTexture_, 0, 0, windowWidth_, windowHeight_);
    }

    // Draw display framebuffer in the designated area with rotation
    if (texture_) {
        glPushMatrix();

        // Calculate center of display area
        float centerX = uiConfig_.display.left + uiConfig_.display.width / 2.0f;
        float centerY = uiConfig_.display.top + uiConfig_.display.height / 2.0f;

        // Translate to center, rotate, translate back
        glTranslatef(centerX, centerY, 0);
        glRotatef(rotation_, 0, 0, 1);

        // Swap width and height for 90° and 270° rotations
        float drawWidth = uiConfig_.display.width;
        float drawHeight = uiConfig_.display.height;
        if (rotation_ == 90 || rotation_ == 270) {
            std::swap(drawWidth, drawHeight);
        }

        glTranslatef(-drawWidth / 2.0f, -drawHeight / 2.0f, 0);
        DrawTexturedQuad(texture_, 0, 0, drawWidth, drawHeight);
        glPopMatrix();
    }

    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);

    glfwSwapBuffers(window_);
}

void GLFWDisplay::PollEvents() {
    glfwPollEvents();
    RenderFramebuffer();
    if (onFrameStartCallback_) {
        onFrameStartCallback_(*this);
    }
}

bool GLFWDisplay::ShouldClose() const {
    return window_ && glfwWindowShouldClose(window_);
}

void GLFWDisplay::SwapBuffers() {
    if (window_) {
        glfwSwapBuffers(window_);
    }
}

void GLFWDisplay::SetDisplayRotation(int degrees) {
    rotation_ = degrees % 360;
    if (rotation_ < 0) rotation_ += 360;
}

void GLFWDisplay::MouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    auto* display = static_cast<GLFWDisplay*>(glfwGetWindowUserPointer(window));
    if (display) {
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);
        display->HandleMouseButton(button, action, xpos, ypos);
    }
}

void GLFWDisplay::HandleMouseButton(int button, int action, double x, double y) {
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    // Check if click is within any button
    for (const auto& btn : uiConfig_.buttons) {
        if (x >= btn.left && x < btn.left + btn.width &&
            y >= btn.top && y < btn.top + btn.height) {
            if (action == GLFW_PRESS) {
                OnKeyPressed(btn.bank, btn.index);
            } else if (action == GLFW_RELEASE) {
                OnKeyReleased(btn.bank, btn.index);
            }
        }
    }
}

void GLFWDisplay::KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action != GLFW_PRESS && action != GLFW_RELEASE) return;
    auto* display = static_cast<GLFWDisplay*>(glfwGetWindowUserPointer(window));
    if (display) {
        display->HandleKeyboard(key, action);
    }
}

void GLFWDisplay::HandleKeyboard(int key, int action) {
    if (action != GLFW_PRESS && action != GLFW_RELEASE) return;

    // Check if key matches any keycap
    for (const auto& keycap : uiConfig_.keycaps) {
        if (key == keycap.glfwKey) {
            if (action == GLFW_PRESS) {
                OnKeyPressed(keycap.bank, keycap.index);
            } else if (action == GLFW_RELEASE) {
                OnKeyReleased(keycap.bank, keycap.index);
            }
            break;
        }
    }
}

int GLFWDisplay::MapKeyNameToGLFW(const std::string& keyName) {
    // Map common key names to GLFW key codes
    if (keyName == "shift") return GLFW_KEY_LEFT_SHIFT;
    if (keyName == "ctrl" || keyName == "control") return GLFW_KEY_LEFT_CONTROL;
    if (keyName == "alt") return GLFW_KEY_LEFT_ALT;
    if (keyName == "space") return GLFW_KEY_SPACE;
    if (keyName == "enter" || keyName == "return") return GLFW_KEY_ENTER;
    if (keyName == "tab") return GLFW_KEY_TAB;
    if (keyName == "escape" || keyName == "esc") return GLFW_KEY_ESCAPE;
    if (keyName == "backspace") return GLFW_KEY_BACKSPACE;

    // Arrow keys
    if (keyName == "up") return GLFW_KEY_UP;
    if (keyName == "down") return GLFW_KEY_DOWN;
    if (keyName == "left") return GLFW_KEY_LEFT;
    if (keyName == "right") return GLFW_KEY_RIGHT;

    // Single character keys (a-z, 0-9)
    if (keyName.length() == 1) {
        char c = keyName[0];
        if (c >= 'a' && c <= 'z') return GLFW_KEY_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return GLFW_KEY_A + (c - 'A');
        if (c >= '0' && c <= '9') return GLFW_KEY_0 + (c - '0');
    }

    // Default: return the key name as-is if it's already a valid GLFW key code
    return GLFW_KEY_UNKNOWN;
}
