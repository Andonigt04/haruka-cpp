#include <cstdlib>
#include "logger.h"
#include <ctime>
#include <cstring>

namespace Haruka {

static LogLevel s_level = LogLevel::Debug;

void setLogLevel(LogLevel level) { s_level = level; }

// Se lee UNA vez: es un interruptor de sesión, no algo que cambie a mitad de partida, y consultar
// el entorno en cada frame por cada traza sería peor que la traza.
bool diagLogs() {
    static const bool s_on = [] {
        const char* e = std::getenv("HARUKA_DIAG");
        return e && e[0] == '1';
    }();
    return s_on;
}
LogLevel getLogLevel() { return s_level; }

static const char* levelLabel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

static const char* levelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "\033[90m";  // bright black (gray)
        case LogLevel::Info:  return "\033[92m";  // green
        case LogLevel::Warn:  return "\033[93m";  // yellow
        case LogLevel::Error: return "\033[91m";  // red
    }
    return "\033[0m";
}

void vlog(LogLevel level, const char* component, const char* fmt, va_list args) {
    if (level < s_level) return;

    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_r(&t, &tm);
    char timeBuf[16];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm);

    std::fprintf(stderr, "%s[%s] [%s]%s",
                 levelColor(level), timeBuf, levelLabel(level), "\033[0m");
    if (component && component[0])
        std::fprintf(stderr, " [%s]", component);
    std::fprintf(stderr, " ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
}

void log(LogLevel level, const char* component, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level, component, fmt, args);
    va_end(args);
}

} // namespace Haruka
