#include "headless.h"
#include "utils/log.h"
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

HeadlessFrontend::HeadlessFrontend(const std::string& configPath, int inputFd) {
    std::ifstream config(configPath);
    if (!config) throw std::runtime_error("Failed to open key config: " + configPath);
    nlohmann::json json;
    config >> json;
    // Use the same logical button names and wiring as mouse input, including
    // sharps (f3#) and encoder push buttons. No GLFW or image loading here.
    for (const auto& [name, button] : json.at("buttons").items()) {
        const auto& gpio = button.at("gpio");
        int bank = gpio.at(0).get<int>();
        int index = gpio.at(1).get<int>();
        if (bank < 0 || bank >= 8 || index < 0 || index >= 16)
            throw std::runtime_error("Invalid GPIO for key: " + name);
        keys_.emplace(name, Pin{bank, index});
    }
    inputFd_ = dup(inputFd);
    if (inputFd_ < 0) throw std::runtime_error("Failed to open headless input");
}

HeadlessFrontend::~HeadlessFrontend() {
    if (inputFd_ >= 0) close(inputFd_);
}

bool HeadlessFrontend::ReadLine(std::string& line) {
    auto end = pending_.find('\n');
    if (end == std::string::npos && !eof_) {
        // Do not use getline on stdin: a partial pipe line must not block
        // frame pulses, a timed release, or SIGTERM shutdown. There is one
        // reader, and read() is only called after poll() reports readiness.
        pollfd fd{inputFd_, POLLIN, 0};
        int ready = poll(&fd, 1, 0);
        if (ready < 0 && errno != EINTR)
            throw std::runtime_error("Failed to poll headless input");
        if (ready <= 0) return false;
        if (fd.revents & (POLLERR | POLLNVAL))
            throw std::runtime_error("Headless input descriptor error");
        if (!(fd.revents & (POLLIN | POLLHUP))) return false;
        char buffer[4096];
        auto count = read(inputFd_, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) return false;
            throw std::runtime_error(std::string("Failed to read headless input: ") + std::strerror(errno));
        }
        if (count == 0) eof_ = true;
        else pending_.append(buffer, static_cast<size_t>(count));
        end = pending_.find('\n');
    }
    constexpr size_t maxLine = 4096;
    if ((end == std::string::npos && pending_.size() > maxLine) ||
        (end != std::string::npos && end > maxLine))
        throw std::runtime_error("Headless command exceeds 4096 bytes");
    if (end == std::string::npos) {
        if (!eof_ || pending_.empty()) return false;
        end = pending_.size(); // Accept the final line without a newline.
    }
    line = pending_.substr(0, end);
    pending_.erase(0, end < pending_.size() ? end + 1 : end);
    return true;
}

static int Duration(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Duration must be an integer in milliseconds (1..86400000)");
    unsigned long duration = std::stoul(value);
    if (duration == 0 || duration > 86400000)
        throw std::runtime_error("Duration must be in 1..86400000 milliseconds");
    return static_cast<int>(duration);
}

void HeadlessFrontend::Execute(const std::string& line, Clock::time_point now) {
    std::istringstream stream(line);
    std::vector<std::string> args;
    for (std::string word; stream >> word;) args.push_back(word);
    if (args.empty() || args[0][0] == '#') return;
    const auto& command = args[0];
    if (command == "quit" && args.size() == 1) {
        shouldClose_ = true;
    } else if (command == "keys" && args.size() == 1) {
        for (const auto& [name, pin] : keys_) std::cout << name << '\n';
        std::cout.flush();
    } else if (command == "wait" && args.size() == 2) {
        resumeAt_ = now + std::chrono::milliseconds(Duration(args[1]));
    } else if (((command == "press" || command == "release") && args.size() == 2) ||
               (command == "tap" && (args.size() == 2 || args.size() == 3))) {
        auto key = keys_.find(args[1]);
        if (key == keys_.end()) throw std::runtime_error("Unknown key: " + args[1]);
        int duration = command == "tap" ? (args.size() == 3 ? Duration(args[2]) : 100) : 0;
        auto [bank, index] = key->second;
        if (command == "release") OnKeyReleased(bank, index);
        else OnKeyPressed(bank, index);
        LogInfo("Headless input queued: %s %s", command.c_str(), args[1].c_str());
        if (command == "tap") {
            releaseAtResume_ = key->second;
            resumeAt_ = now + std::chrono::milliseconds(duration);
        }
    } else {
        throw std::runtime_error("Invalid headless command: " + line);
    }
}

void HeadlessFrontend::PollEvents(Clock::time_point now) {
    // This callback drives PORTG3 frame sync; removing it can leave guest
    // display code waiting forever even though we do not render its pixels.
    if (frameCallback_) frameCallback_(*this);
    if (now < resumeAt_ || shouldClose_) return;
    if (releaseAtResume_) {
        OnKeyReleased(releaseAtResume_->first, releaseAtResume_->second);
        releaseAtResume_.reset();
        // Give the CPU an opportunity to observe this edge before a following
        // command (especially another tap of the same key or quit).
        return;
    }
    // Bound work per tick even for an endless stream of commands/comments.
    for (int budget = 0; budget < 64; ++budget) {
        std::string line;
        if (!ReadLine(line)) break;
        Execute(line, now);
        if (now < resumeAt_ || shouldClose_) break;
    }
}
