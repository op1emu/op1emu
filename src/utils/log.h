#pragma once

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
};

void SetLogLevel(LogLevel level);

void Log(LogLevel level, const char* format, ...);
void LogDebug(const char* format, ...);
void LogInfo(const char* format, ...);
void LogWarn(const char* format, ...);
void LogError(const char* format, ...);