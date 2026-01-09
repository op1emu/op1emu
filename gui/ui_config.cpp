#include "ui_config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

UIConfig UIConfig::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open UI config: " + path);
    }

    json j;
    file >> j;

    UIConfig config;
    config.background = j["background"];
    config.display.left = j["display"]["left"];
    config.display.top = j["display"]["top"];
    config.display.width = j["display"]["width"];
    config.display.height = j["display"]["height"];

    return config;
}
