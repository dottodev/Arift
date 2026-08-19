#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace arift {

enum class LogLevel : int {
    kTrace = 0,
    kDebug = 1,
    kInfo = 2,
    kWarn = 3,
    kError = 4,
    kOff = 5,
};

class Logger {
public:
    static Logger& instance();

    void setLevel(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    void setTagPrefix(const char* prefix) { tag_prefix_ = prefix; }

    void vlog(LogLevel level, const char* tag, const char* fmt, va_list args);
    void log(LogLevel level, const char* tag, const char* fmt, ...);

    void trace(const char* tag, const char* fmt, ...);
    void debug(const char* tag, const char* fmt, ...);
    void info(const char* tag, const char* fmt, ...);
    void warn(const char* tag, const char* fmt, ...);
    void error(const char* tag, const char* fmt, ...);

    uint64_t logCount() const { return count_; }
    uint64_t errorCount() const { return error_count_; }

private:
    Logger() = default;
    LogLevel level_ = LogLevel::kDebug;
    const char* tag_prefix_ = "ARIFT";
    uint64_t count_ = 0;
    uint64_t error_count_ = 0;
};

#define ARIFT_LOG(level, tag, ...) \
    ::arift::Logger::instance().log(::arift::LogLevel::level, tag, __VA_ARGS__)

#define ARIFT_TRACE(tag, ...) ARIFT_LOG(kTrace, tag, __VA_ARGS__)
#define ARIFT_DEBUG(tag, ...) ARIFT_LOG(kDebug, tag, __VA_ARGS__)
#define ARIFT_INFO(tag, ...)  ARIFT_LOG(kInfo, tag, __VA_ARGS__)
#define ARIFT_WARN(tag, ...)  ARIFT_LOG(kWarn, tag, __VA_ARGS__)
#define ARIFT_ERROR(tag, ...) ARIFT_LOG(kError, tag, __VA_ARGS__)

constexpr const char* kTagCore = "AriftCore";
constexpr const char* kTagBridge = "AriftBridge";
constexpr const char* kTagEsp = "AriftEsp";
constexpr const char* kTagMapHack = "AriftMapHack";
constexpr const char* kTagRankBooster = "AriftRankBooster";
constexpr const char* kTagEnemyLag = "AriftEnemyLag";
constexpr const char* kTagVoidBan = "AriftVoidBan";
constexpr const char* kTagAutoRetri = "AriftAutoRetri";
constexpr const char* kTagAutoAim = "AriftAutoAim";
constexpr const char* kTagTankDefense = "AriftTankDefense";
constexpr const char* kTagPhysicalDamage = "AriftPhysicalDamage";
constexpr const char* kTagMemory = "AriftMemory";
constexpr const char* kTagHooks = "AriftHooks";
constexpr const char* kTagLoader = "AriftLoader";
constexpr const char* kTagGuard = "AriftGuard";

}  // namespace arift