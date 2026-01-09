#pragma once
#include <string>

struct DisplayRect {
    int left;
    int top;
    int width;
    int height;
};

struct UIConfig {
    std::string background;
    DisplayRect display;

    static UIConfig load(const std::string& path);
};
