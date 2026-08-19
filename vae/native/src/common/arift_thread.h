#pragma once

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace arift {

class Thread {
public:
    using Func = std::function<void()>;

    explicit Thread(std::string name = "arift-thread");
    ~Thread();

    bool start(Func fn);
    bool join();
    bool detach();
    bool isRunning() const { return running_.load(); }
    void setName(const std::string& name);

    static void sleepMs(int64_t ms);
    static void yield();

private:
    pthread_t handle_ = 0;
    std::string name_;
    std::atomic<bool> running_{false};
};

// Reusable event flag with timed wait (used by feature loops).
class EventFlag {
public:
    void set();
    void clear();
    bool wait(int64_t timeoutMs);
    bool isSet() const { return set_.load(); }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> set_{false};
};

// Tiny spinlock for ultra-low-latency sections.
class SpinLock {
public:
    void lock();
    void unlock();

private:
    std::atomic<bool> locked_{false};
};

class SpinGuard {
public:
    explicit SpinGuard(SpinLock& lock) : lock_(lock) { lock_.lock(); }
    ~SpinGuard() { lock_.unlock(); }

private:
    SpinLock& lock_;
};

}  // namespace arift