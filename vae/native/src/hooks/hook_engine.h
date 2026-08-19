#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace arift {

// Hook type enum shared by all hook engines.
enum class HookType : int {
    kInline = 0,
    kPlt = 1,
    kJni = 2,
};

struct HookRecord {
    HookType type = HookType::kInline;
    std::string name;
    uintptr_t target = 0;
    uintptr_t replacement = 0;
    uintptr_t trampoline = 0;
    bool active = false;
    std::vector<uint32_t> originalBytes;
};

// Central registry for every installed hook. Enables listing, toggling and
// diagnostics from one place (used by guard/telemetry).
class HookEngine {
public:
    static HookEngine& instance();

    bool registerHook(HookRecord rec);
    bool unregisterHook(uintptr_t target);
    bool unregisterByName(const std::string& name);
    HookRecord* find(uintptr_t target);
    HookRecord* findByName(const std::string& name);

    void setActive(uintptr_t target, bool active);
    bool isActive(uintptr_t target) const;

    size_t count() const;
    std::vector<const HookRecord*> all() const;

    // Global kill-switch: disables everything (emergency stealth).
    void panicDisableAll();
    bool panicTriggered() const { return panic_; }

    void setPanicCallback(std::function<void()> cb) { panic_cb_ = std::move(cb); }

private:
    HookEngine() = default;
    mutable std::mutex mutex_;
    std::map<uintptr_t, HookRecord> hooks_;
    bool panic_ = false;
    std::function<void()> panic_cb_;
};

}  // namespace arift