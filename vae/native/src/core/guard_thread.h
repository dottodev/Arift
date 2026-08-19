#pragma once

#include <atomic>
#include <cstdint>

#include "arift_thread.h"

namespace arift {

// Background watchdog: monitors core health, reacts to integrity alarms,
// and provides heartbeat ticks to the guard/anti-detection layer.
class GuardThread {
public:
    static GuardThread& instance();

    void start();
    void stop();
    bool running() const { return running_.load(); }

    uint64_t heartbeatCount() const { return heartbeats_.load(); }
    uint64_t alarms() const { return alarms_.load(); }

    // Signal an integrity alarm (detected by any module).
    void raiseAlarm(int code, const char* detail);

    // Last alarm info.
    int lastAlarmCode() const { return last_alarm_code_.load(); }
    const char* lastAlarmDetail() const { return last_alarm_detail_; }

    // Tune interval (ms).
    void setIntervalMs(int64_t ms) { interval_ms_ = ms; }

private:
    GuardThread() = default;
    ~GuardThread() { stop(); }

    void loop();

    Thread thread_{"arift-guard"};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> heartbeats_{0};
    std::atomic<uint64_t> alarms_{0};
    std::atomic<int> last_alarm_code_{0};
    char last_alarm_detail_[256] = {0};
    int64_t interval_ms_ = 1000;
};

}  // namespace arift