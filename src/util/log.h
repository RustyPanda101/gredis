// tiny stderr logger: timestamp + level + file:line. wall clock only,
// TTL/timer code uses clock.h's monotonic clock instead.
#pragma once

namespace gredis {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

extern LogLevel g_log_level; // minimum level printed, set at startup from Config::log_level

#if defined(__GNUC__) || defined(__clang__)
#define GREDIS_PRINTF_ATTR(fmt_idx, first_arg_idx) \
    __attribute__((format(printf, fmt_idx, first_arg_idx)))
#else
#define GREDIS_PRINTF_ATTR(fmt_idx, first_arg_idx)
#endif

void log_line(LogLevel level, const char* file, int line, const char* fmt, ...)
    GREDIS_PRINTF_ATTR(4, 5);

#undef GREDIS_PRINTF_ATTR

} // namespace gredis

#define LOG_DEBUG(...) \
    ::gredis::log_line(::gredis::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...) \
    ::gredis::log_line(::gredis::LogLevel::Info, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...) \
    ::gredis::log_line(::gredis::LogLevel::Warn, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) \
    ::gredis::log_line(::gredis::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
