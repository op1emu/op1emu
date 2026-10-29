#include "svg_theme.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
constexpr std::array<const char *, 4> roles = {"body", "panel", "keycap", "encoder-ring"};
bool IsColor(const std::string &value) {
    return value.size() == 7 && value[0] == '#' &&
           std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isxdigit(c); });
}
} // namespace

std::string SvgTheme::Resolve(const std::string &nameOrPath) {
    for (const auto *name : {"slate", "silver", "yellow", "green", "purple", "orange"})
        if (nameOrPath == name)
            return "gui/themes/" + nameOrPath + ".json";
    return nameOrPath;
}

SvgTheme SvgTheme::Load(const std::string &path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Cannot open SVG theme: " + path);
    nlohmann::json j;
    input >> j;
    if (j.at("version") != 1)
        throw std::runtime_error("Unsupported SVG theme version");
    SvgTheme theme;
    theme.name = j.at("name").get<std::string>();
    const auto &palette = j.at("colors");
    if (!palette.is_object() || palette.size() != roles.size())
        throw std::runtime_error("Theme needs body, panel, keycap and encoder-ring colors");
    for (const auto *role : roles) {
        const auto value = palette.at(role).get<std::string>();
        if (!IsColor(value))
            throw std::runtime_error("Theme color must be #RRGGBB: " + std::string(role));
        theme.colors.emplace(role, value);
    }
    return theme;
}

std::string SvgTheme::Stylesheet() const {
    std::string css;
    for (const auto *role : roles) {
        const auto &color = colors.at(role);
        if (!IsColor(color))
            throw std::runtime_error("Invalid SVG theme color");
        css += ".op1-" + std::string(role) + " { fill: " + color + "; }\n";
    }
    return css;
}
