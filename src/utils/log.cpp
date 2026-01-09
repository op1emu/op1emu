#include "log.h"
#include <vector>
#include <map>
#include <cstdarg>
#include <spdlog/spdlog.h>

namespace spd = spdlog;

const std::map<LogLevel, spd::level::level_enum> levelConverter = {
    {LogLevel::Debug, spd::level::level_enum::debug},
    {LogLevel::Info, spd::level::level_enum::info},
    {LogLevel::Warn, spd::level::level_enum::warn},
    {LogLevel::Error, spd::level::level_enum::err},
};

void SetLogLevel(LogLevel level)
{
    auto l = levelConverter.find(level)->second;
    spd::set_level(l);
}

#define BUFFER_SIZE 256

static void LogImpl(LogLevel level, const char* format, va_list args)
{
    auto l = levelConverter.find(level)->second;

    va_list copy;
    va_copy(copy, args);
    auto size = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (size < BUFFER_SIZE) {
        char buffer[BUFFER_SIZE];
        vsnprintf(buffer, BUFFER_SIZE, format, args);
        spd::log(l, buffer);
    } else {
        std::vector<char> buffer(size + 1);
        vsnprintf(buffer.data(), buffer.size(), format, args);
        spd::log(l, buffer.data());
    }
}

void Log(LogLevel level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogImpl(level, format, args);
    va_end(args);
}

#define LOG_IMPL(LEVEL) \
    void Log##LEVEL(const char* format, ...) \
    { \
        va_list args; \
        va_start(args, format); \
        LogImpl(LogLevel::LEVEL, format, args); \
        va_end(args); \
    }

LOG_IMPL(Debug)
LOG_IMPL(Info)
LOG_IMPL(Warn)
LOG_IMPL(Error)