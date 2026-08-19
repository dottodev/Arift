#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arift {

// Precision timing helpers layered on CLOCK_MONOTONIC.
class Stopwatch {
public:
    void start();
    void reset();
    int64_t elapsedMs() const;
    int64_t elapsedUs() const;
    double elapsedSec() const;
    bool running() const { return running_; }

private:
    int64_t start_us_ = 0;
    int64_t acc_us_ = 0;
    bool running_ = false;
};

// Rate limiter: allows an action at most `hz` times per second.
class RateLimiter {
public:
    explicit RateLimiter(double hz);
    bool tryAcquire();
    void setRate(double hz);

private:
    double interval_us_;
    int64_t next_us_ = 0;
};

// Frame-rate governor for overlay loops.
class FrameGovernor {
public:
    explicit FrameGovernor(double targetFps);
    int64_t tick();          // returns sleep needed (us)
    double actualFps() const;

private:
    double interval_us_;
    int64_t last_us_ = 0;
    double fps_ = 0.0;
};

}  // namespace arift