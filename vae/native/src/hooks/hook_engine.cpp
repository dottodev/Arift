#include "hook_engine.h"

#include <algorithm>

#include "arift_log.h"

namespace arift {

HookEngine& HookEngine::instance() {
    static HookEngine engine;
    return engine;
}

bool HookEngine::registerHook(HookRecord rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hooks_.count(rec.target)) {
        ARIFT_WARN(kTagHooks, "Hook already registered @ %p",
                   reinterpret_cast<void*>(rec.target));
        return false;
    }
    hooks_[rec.target] = rec;
    return true;
}

bool HookEngine::unregisterHook(uintptr_t target) {
    std::lock_guard<std::mutex> lock(mutex_);
    return hooks_.erase(target) > 0;
}

bool HookEngine::unregisterByName(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = hooks_.begin(); it != hooks_.end(); ++it) {
        if (it->second.name == name) {
            hooks_.erase(it);
            return true;
        }
    }
    return false;
}

HookRecord* HookEngine::find(uintptr_t target) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hooks_.find(target);
    return it == hooks_.end() ? nullptr : &it->second;
}

HookRecord* HookEngine::findByName(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [target, rec] : hooks_) {
        (void)target;
        if (rec.name == name) return &rec;
    }
    return nullptr;
}

void HookEngine::setActive(uintptr_t target, bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hooks_.find(target);
    if (it != hooks_.end()) it->second.active = active;
}

bool HookEngine::isActive(uintptr_t target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = hooks_.find(target);
    return it != hooks_.end() && it->second.active;
}

size_t HookEngine::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hooks_.size();
}

std::vector<const HookRecord*> HookEngine::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<const HookRecord*> out;
    out.reserve(hooks_.size());
    for (const auto& [target, rec] : hooks_) {
        (void)target;
        out.push_back(&rec);
    }
    std::sort(out.begin(), out.end(),
              [](const HookRecord* a, const HookRecord* b) {
                  return a->target < b->target;
              });
    return out;
}

void HookEngine::panicDisableAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    panic_ = true;
    for (auto& [target, rec] : hooks_) {
        (void)target;
        rec.active = false;
    }
    if (panic_cb_) panic_cb_();
}

}  // namespace arift