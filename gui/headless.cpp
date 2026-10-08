#include "headless.h"
#include "utils/log.h"
#include <nlohmann/json.hpp>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <algorithm>
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

// Strict decimal integer in [low, high]; rejects "", "+1", "1.5", "0x10", overflow.
static long Integer(const std::string& value, long low, long high, const char* what) {
    size_t digits = !value.empty() && value[0] == '-' ? 1 : 0;
    if (value.size() == digits || value.size() > digits + 18 ||
        value.find_first_not_of("0123456789", digits) != std::string::npos)
        throw std::runtime_error(std::string(what) + " must be an integer in " +
                                 std::to_string(low) + ".." + std::to_string(high));
    long number = std::stol(value);
    if (number < low || number > high)
        throw std::runtime_error(std::string(what) + " must be in " +
                                 std::to_string(low) + ".." + std::to_string(high));
    return number;
}

void HeadlessFrontend::Initialize(int width, int height) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (width == width_ && height == height_) return;
    width_ = std::max(width, 0);
    height_ = std::max(height, 0);
    work_.assign(size_t(width_) * height_, 0);
    seen_.assign(work_.size(), 0);
    latest_.clear();
    remaining_ = work_.size();
    started_ = false;
}

void HeadlessFrontend::UpdateRowBuffer(int x, int y, const void* data, int length) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!data || length <= 0 || length % 2 || x < 0 || y < 0 || y >= height_ ||
        x >= width_ || length / 2 > width_ - x)
        return;
    if (x == 0 && y == 0) {
        // A new frame begins at the origin; anything before the first origin
        // is the tail of a frame we did not see from the start.
        started_ = true;
        remaining_ = work_.size();
        std::fill(seen_.begin(), seen_.end(), 0);
    }
    if (!started_) return;
    const size_t begin = size_t(y) * width_ + x, count = length / 2;
    std::memcpy(work_.data() + begin, data, length);
    for (size_t i = begin; i < begin + count; ++i) {
        if (!seen_[i]) {
            seen_[i] = 1;
            --remaining_;
        }
    }
    if (remaining_ == 0) {
        latest_ = work_;
        started_ = false;
        frameCount_.fetch_add(1, std::memory_order_acq_rel);
    }
}

// The device scans the panel portrait (width x height); GLFWDisplay draws it
// rotated 90 degrees clockwise into the landscape UI, so do the same here.
void HeadlessFrontend::WriteScreenshot(const std::string& path) const {
    std::vector<uint16_t> frame;
    int width, height;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        frame = latest_;
        width = width_;
        height = height_;
    }
    if (frame.empty()) throw std::runtime_error("No complete frame yet; use wait-frames first");
    // Rotated image is height x width: pixel (xd, yd) comes from (yd, height-1-xd).
    std::vector<uint8_t> rgb(frame.size() * 3);
    for (int yd = 0; yd < width; ++yd) {
        for (int xd = 0; xd < height; ++xd) {
            const uint16_t p = frame[size_t(height - 1 - xd) * width + yd];
            const unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            uint8_t* out = &rgb[(size_t(yd) * height + xd) * 3];
            out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
            out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
            out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
        }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "P6\n" << height << ' ' << width << "\n255\n";
    file.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    file.flush();
    if (!file) throw std::runtime_error("Failed to write screenshot: " + path);
    LogInfo("Headless screenshot written: %s (%dx%d)", path.c_str(), height, width);
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
    } else if (command == "wait-frames" && (args.size() == 2 || args.size() == 3)) {
        long frames = Integer(args[1], 1, 100000, "Frame count");
        int timeout = args.size() == 3 ? Duration(args[2]) : 600000;
        frameTarget_ = FrameCount() + static_cast<uint64_t>(frames);
        frameDeadline_ = now + std::chrono::milliseconds(timeout);
    } else if (command == "screenshot" && args.size() == 2) {
        WriteScreenshot(args[1]);
    } else if (command == "accel" && args.size() == 2 && args[1] == "random") {
        sensors_.accelFixed = false;
        ++sensorVersion_;
    } else if (command == "accel" && args.size() == 4) {
        const auto ax = Integer(args[1], INT16_MIN, INT16_MAX, "Acceleration");
        const auto ay = Integer(args[2], INT16_MIN, INT16_MAX, "Acceleration");
        const auto az = Integer(args[3], INT16_MIN, INT16_MAX, "Acceleration");
        sensors_.accelFixed = true;
        sensors_.ax = static_cast<int16_t>(ax);
        sensors_.ay = static_cast<int16_t>(ay);
        sensors_.az = static_cast<int16_t>(az);
        ++sensorVersion_;
    } else if (command == "volume" && args.size() == 2) {
        sensors_.volume = static_cast<uint8_t>(Integer(args[1], 0, 255, "Volume"));
        ++sensorVersion_;
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
    if (frameTarget_) {
        if (FrameCount() < *frameTarget_) {
            // A display that never produces a frame must fail the run, not hang it.
            if (now >= frameDeadline_) throw std::runtime_error("Timed out waiting for frames");
            return;
        }
        frameTarget_.reset();
    }
    // Bound work per tick even for an endless stream of commands/comments.
    for (int budget = 0; budget < 64; ++budget) {
        std::string line;
        if (!ReadLine(line)) break;
        Execute(line, now);
        if (now < resumeAt_ || shouldClose_ || frameTarget_) break;
    }
}
