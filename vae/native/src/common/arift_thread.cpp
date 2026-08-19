#include "arift_thread.h"

#include <sched.h>
#include <unistd.h>

#include "arift_log.h"

namespace arift {

Thread::Thread(std::string name) : name_(std::move(name)) {}

Thread::~Thread() {
    if (running_.load()) {
        detach();
    }
}

bool Thread::start(Func fn) {
    if (running_.load()) return false;
    running_.store(true);
    auto* holder = new std::pair<Thread*, Func>(this, std::move(fn));
    int rc = pthread_create(&handle_, nullptr, [](void* arg) -> void* {
        auto* pair = static_cast<std::pair<Thread*, Func>*>(arg);
        Thread* self = pair->first;
        Func fn = std::move(pair->second);
        delete pair;
        if (pthread_setname_np(pthread_self(), self->name_.substr(0, 15).c_str()) != 0) {
            ARIFT_DEBUG(kTagCore, "setname failed for %s", self->name_.c_str());
        }
        fn();
        self->running_.store(false);
        return nullptr;
    }, holder);
    if (rc != 0) {
        running_.store(false);
        delete holder;
        return false;
    }
    return true;
}

bool Thread::join() {
    if (!running_.load()) return true;
    void* ret = nullptr;
    return pthread_join(handle_, &ret) == 0;
}

bool Thread::detach() {
    if (!running_.load()) return true;
    return pthread_detach(handle_) == 0;
}

void Thread::setName(const std::string& name) {
    name_ = name;
}

void Thread::sleepMs(int64_t ms) {
    if (ms <= 0) {
        sched_yield();
        return;
    }
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(ms / 1000);
    ts.tv_nsec = static_cast<long>((ms % 1000) * 1000000L);
    nanosleep(&ts, nullptr);
}

void Thread::yield() {
    sched_yield();
}

void EventFlag::set() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        set_.store(true);
    }
    cv_.notify_all();
}

void EventFlag::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    set_.store(false);
}

bool EventFlag::wait(int64_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (set_.load()) return true;
    if (timeoutMs < 0) {
        cv_.wait(lock, [this] { return set_.load(); });
        return true;
    }
    return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this] { return set_.load(); });
}

void SpinLock::lock() {
    bool expected = false;
    while (!locked_.compare_exchange_weak(expected, true, std::memory_order_acquire)) {
        expected = false;
        sched_yield();
    }
}

void SpinLock::unlock() {
    locked_.store(false, std::memory_order_release);
}

}  // namespace arift