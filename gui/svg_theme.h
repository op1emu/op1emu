#pragma once
#include <map>
#include <string>

// Paint-only palettes: geometry, filter data and firmware pixels are separate.
struct SvgTheme {
    std::string name;
    std::map<std::string, std::string> colors;
    static SvgTheme Load(const std::string &path);
    static std::string Resolve(const std::string &nameOrPath);
    std::string Stylesheet() const;
};
