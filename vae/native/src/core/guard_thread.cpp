#include "guard_thread.h"

#include <cstring>

#include "arift_log.h"
#include "arift_utils.h"
#include "arm64_patch.h"
#include "cheat_registry.h"
#include "hook_engine.h"

namespace arift {

GuardThread& GuardThread::instance() {
    static GuardThread guard;
    return guard;
}

void GuardThread::start() {
    if (running_.load()) return;
    running_.store(true);
    if (!thread_.start([this] { loop(); })) {
        running_.store(false);
        ARIFT_ERROR(kTagGuard, "Guard thread failed to start");
    }
}

void GuardThread::stop() {
    if (!running_.load()) return;
    running_.store(false);
    thread_.join();
}

void GuardThread::loop() {
    ARIFT_INFO(kTagGuard, "Guard thread online");
    while (running_.load()) {
        heartbeats_.fetch_add(1);

        // Health check: if no module is running but features are active,
        // something is broken — log it.
        if (CheatRegistry::instance().runningCount() == 0 &&
            HookEngine::instance().count() > 0) {
            ARIFT_WARN(kTagGuard, "Hooks installed but no module running");
        }

        // Hook integrity: periodically verify trampolines are still present.
        for (const auto* rec : HookEngine::instance().all()) {
            if (!rec->active) continue;
            if (rec->type == HookType::kInline) {
                uint32_t first = Arm64Patch::readInsn(rec->target);
                if ((first & 0xFC000000) != 0x14000000) {
                    raiseAlarm(0x11, "inline hook trampoline missing");
                }
            }
        }

        Thread::sleepMs(interval_ms_);
    }
    ARIFT_INFO(kTagGuard, "Guard thread offline");
}

void GuardThread::raiseAlarm(int code, const char* detail) {
    alarms_.fetch_add(1);
    last_alarm_code_.store(code);
    if (detail) {
        strncpy(last_alarm_detail_, detail, sizeof(last_alarm_detail_) - 1);
        last_alarm_detail_[sizeof(last_alarm_detail_) - 1] = 0;
    }
    ARIFT_WARN(kTagGuard, "Alarm %d: %s", code, detail ? detail : "");
}

}  // namespace arift