#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace arift {

// Every cheat module registers itself here. The registry drives lifecycle,
// telemetry and the guard's integrity checks.
struct ModuleHandle {
    std::string name;
    int featureId = 0;
    std::function<int()> onStart;
    std::function<int()> onStop;
    std::function<void()> onTick;          // called each frame while active
    std::function<std::string()> onDiag;
    uint64_t startedAtMs = 0;
    bool running = false;
};

class CheatRegistry {
public:
    static CheatRegistry& instance();

    bool registerModule(ModuleHandle handle);
    bool unregisterModule(const std::string& name);
    ModuleHandle* find(const std::string& name);

    int start(const std::string& name);
    int stop(const std::string& name);
    bool isRunning(const std::string& name) const;

    void tickAll();                 // tick every running module
    void stopAll();                 // emergency stop

    size_t count() const;
    size_t runningCount() const;
    std::map<std::string, ModuleHandle>& modules() { return modules_; }

    std::string dump() const;

private:
    CheatRegistry() = default;
    std::map<std::string, ModuleHandle> modules_;
};

}  // namespace arift