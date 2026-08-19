#include "cheat_registry.h"

#include <algorithm>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {

CheatRegistry& CheatRegistry::instance() {
    static CheatRegistry registry;
    return registry;
}

bool CheatRegistry::registerModule(ModuleHandle handle) {
    if (handle.name.empty() || handle.onStart == nullptr || handle.onStop == nullptr) {
        ARIFT_ERROR(kTagCore, "Bad module registration: %s", handle.name.c_str());
        return false;
    }
    if (modules_.count(handle.name)) {
        ARIFT_WARN(kTagCore, "Module already registered: %s", handle.name.c_str());
        return false;
    }
    modules_[handle.name] = std::move(handle);
    ARIFT_DEBUG(kTagCore, "Registered module: %s (feature=%d)",
                modules_[handle.name].name.c_str(), modules_[handle.name].featureId);
    return true;
}

bool CheatRegistry::unregisterModule(const std::string& name) {
    auto it = modules_.find(name);
    if (it == modules_.end()) return false;
    if (it->second.running) {
        it->second.onStop();
    }
    modules_.erase(it);
    return true;
}

ModuleHandle* CheatRegistry::find(const std::string& name) {
    auto it = modules_.find(name);
    return it == modules_.end() ? nullptr : &it->second;
}

int CheatRegistry::start(const std::string& name) {
    auto* m = find(name);
    if (!m) return -1;
    if (m->running) return 0;
    int rc = m->onStart();
    if (rc != 0) return rc;
    m->running = true;
    m->startedAtMs = utils::monotonicMs();
    ARIFT_INFO(kTagCore, "Module started: %s", name.c_str());
    return 0;
}

int CheatRegistry::stop(const std::string& name) {
    auto* m = find(name);
    if (!m) return -1;
    if (!m->running) return 0;
    int rc = m->onStop();
    m->running = false;
    ARIFT_INFO(kTagCore, "Module stopped: %s", name.c_str());
    return rc;
}

bool CheatRegistry::isRunning(const std::string& name) const {
    auto it = modules_.find(name);
    return it != modules_.end() && it->second.running;
}

void CheatRegistry::tickAll() {
    for (auto& [name, m] : modules_) {
        (void)name;
        if (m.running && m.onTick) {
            m.onTick();
        }
    }
}

void CheatRegistry::stopAll() {
    for (auto& [name, m] : modules_) {
        (void)name;
        if (m.running) {
            m.onStop();
            m.running = false;
        }
    }
}

size_t CheatRegistry::count() const {
    return modules_.size();
}

size_t CheatRegistry::runningCount() const {
    size_t n = 0;
    for (const auto& [name, m] : modules_) {
        (void)name;
        if (m.running) ++n;
    }
    return n;
}

std::string CheatRegistry::dump() const {
    std::string out;
    for (const auto& [name, m] : modules_) {
        out += name + ": " + (m.running ? "RUNNING" : "idle") +
               " (feature=" + std::to_string(m.featureId) + ")\n";
    }
    return out;
}

}  // namespace arift