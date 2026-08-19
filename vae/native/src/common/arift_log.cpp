#include "arift_log.h"

#include <android/log.h>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace arift {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::vlog(LogLevel level, const char* tag, const char* fmt, va_list args) {
    if (level < level_) return;
    ++count_;
    if (level == LogLevel::kError) ++error_count_;

    char message[2048];
    vsnprintf(message, sizeof(message), fmt, args);

    char final_tag[128];
    snprintf(final_tag, sizeof(final_tag), "%s/%s", tag_prefix_, tag ? tag : "?");

    android_LogPriority prio = ANDROID_LOG_DEBUG;
    switch (level) {
        case LogLevel::kTrace:  prio = ANDROID_LOG_VERBOSE; break;
        case LogLevel::kDebug:  prio = ANDROID_LOG_DEBUG; break;
        case LogLevel::kInfo:   prio = ANDROID_LOG_INFO; break;
        case LogLevel::kWarn:   prio = ANDROID_LOG_WARN; break;
        case LogLevel::kError:  prio = ANDROID_LOG_ERROR; break;
        case LogLevel::kOff:    prio = ANDROID_LOG_SILENT; break;
    }
    __android_log_write(prio, final_tag, message);

    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%llu.%03llu] %s: %s\n",
            static_cast<unsigned long long>(ts.tv_sec),
            static_cast<unsigned long long>(ts.tv_nsec / 1000000ULL),
            final_tag, message);
}

void Logger::log(LogLevel level, const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(level, tag, fmt, args);
    va_end(args);
}

void Logger::trace(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(LogLevel::kTrace, tag, fmt, args);
    va_end(args);
}

void Logger::debug(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(LogLevel::kDebug, tag, fmt, args);
    va_end(args);
}

void Logger::info(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(LogLevel::kInfo, tag, fmt, args);
    va_end(args);
}

void Logger::warn(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(LogLevel::kWarn, tag, fmt, args);
    va_end(args);
}

void Logger::error(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vlog(LogLevel::kError, tag, fmt, args);
    va_end(args);
}

}  // namespace arift