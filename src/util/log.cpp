#include "util/log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace gredis {

LogLevel g_log_level = LogLevel::Info;

namespace {

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

} // namespace

void log_line(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (level < g_log_level) {
        return;
    }

    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);

    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    std::fprintf(stderr, "%s.%03lld [%s] ", timebuf, static_cast<long long>(ms), level_name(level));

    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);

    std::fprintf(stderr, " (%s:%d)\n", file, line);
}

} // namespace gredis
