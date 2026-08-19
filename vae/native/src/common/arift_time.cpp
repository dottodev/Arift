#include "arift_time.h"

#include <time.h>

#include "arift_utils.h"

namespace arift {

namespace {
int64_t nowUs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}
}  // namespace

void Stopwatch::start() {
    start_us_ = nowUs();
    running_ = true;
}

void Stopwatch::reset() {
    acc_us_ = 0;
    start_us_ = nowUs();
    running_ = true;
}

int64_t Stopwatch::elapsedUs() const {
    if (!running_) return acc_us_;
    return acc_us_ + (nowUs() - start_us_);
}

int64_t Stopwatch::elapsedMs() const {
    return elapsedUs() / 1000;
}

double Stopwatch::elapsedSec() const {
    return static_cast<double>(elapsedUs()) / 1000000.0;
}

RateLimiter::RateLimiter(double hz) {
    setRate(hz);
}

void RateLimiter::setRate(double hz) {
    interval_us_ = hz > 0 ? 1000000.0 / hz : 0.0;
}

bool RateLimiter::tryAcquire() {
    int64_t now = nowUs();
    if (now < next_us_) return false;
    next_us_ = now + static_cast<int64_t>(interval_us_);
    return true;
}

FrameGovernor::FrameGovernor(double targetFps) : interval_us_(1000000.0 / targetFps) {}

int64_t FrameGovernor::tick() {
    int64_t now = nowUs();
    int64_t elapsed = now - last_us_;
    int64_t sleep = 0;
    if (elapsed < static_cast<int64_t>(interval_us_)) {
        sleep = static_cast<int64_t>(interval_us_) - elapsed;
        struct timespec ts;
        ts.tv_sec = sleep / 1000000;
        ts.tv_nsec = (sleep % 1000000) * 1000;
        nanosleep(&ts, nullptr);
    }
    int64_t after = nowUs();
    int64_t span = after - last_us_;
    last_us_ = after;
    if (span > 0) {
        fps_ = 1000000.0 / static_cast<double>(span);
    }
    return sleep;
}

double FrameGovernor::actualFps() const {
    return fps_;
}

}  // namespace arift