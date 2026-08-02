#pragma once
#include <cstdio>
#include <cstdarg>

namespace Haruka {

enum class LogLevel {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

void setLogLevel(LogLevel level);
LogLevel getLogLevel();

void vlog(LogLevel level, const char* component, const char* fmt, va_list args);
void log(LogLevel level, const char* component, const char* fmt, ...);

} // namespace Haruka

#define HARUKA_LOGD(comp, ...)  Haruka::log(Haruka::LogLevel::Debug, (comp), __VA_ARGS__)
#define HARUKA_LOGI(comp, ...)  Haruka::log(Haruka::LogLevel::Info,  (comp), __VA_ARGS__)
#define HARUKA_LOGW(comp, ...)  Haruka::log(Haruka::LogLevel::Warn,  (comp), __VA_ARGS__)
#define HARUKA_LOGE(comp, ...)  Haruka::log(Haruka::LogLevel::Error, (comp), __VA_ARGS__)
