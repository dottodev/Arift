#include "injection_flow.h"

#include "arift_log.h"
#include "arift_time.h"
#include "arift_utils.h"
#include "cheat_registry.h"
#include "feature_switch.h"
#include "hook_engine.h"
#include "memory_map.h"
#include "memory_scanner.h"

namespace arift {

InjectionFlow& InjectionFlow::instance() {
    static InjectionFlow flow;
    return flow;
}

int InjectionFlow::run(int pid, uintptr_t libBase) {
    if (active_) return -1;

    MemoryMap::instance().refresh(pid);
    ARIFT_INFO(kTagCore, "Memory map refreshed: %zu regions",
               MemoryMap::instance().regions().size());

    active_ = true;
    started_at_ms_ = utils::monotonicMs();
    frames_ = 0;
    ARIFT_INFO(kTagCore, "Injection flow active (pid=%d base=%llx)",
               pid, static_cast<unsigned long long>(libBase));
    return 0;
}

void InjectionFlow::pump() {
    if (!active_) return;
    ++frames_;
    CheatRegistry::instance().tickAll();
}

int InjectionFlow::teardown() {
    if (!active_) return 0;
    CheatRegistry::instance().stopAll();
    HookEngine::instance().panicDisableAll();
    active_ = false;
    ARIFT_INFO(kTagCore, "Injection flow torn down (%llu frames)",
               static_cast<unsigned long long>(frames_));
    return 0;
}

}  // namespace arift