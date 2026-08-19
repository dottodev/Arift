#pragma once

#include <cstdint>

namespace arift {

// Orchestrates the full injection lifecycle:
//   prepare -> attach -> enable modules -> run loops -> detach
class InjectionFlow {
public:
    static InjectionFlow& instance();

    // Boot sequence: open target memory, refresh maps, install hooks,
    // start modules whose features are enabled.
    int run(int pid, uintptr_t libBase);

    // Per-frame pump: ticks registered modules and the guard.
    void pump();

    // Tear everything down cleanly.
    int teardown();

    bool active() const { return active_; }
    int64_t startedAtMs() const { return started_at_ms_; }
    uint64_t frames() const { return frames_; }

private:
    InjectionFlow() = default;
    bool active_ = false;
    int64_t started_at_ms_ = 0;
    uint64_t frames_ = 0;
};

}  // namespace arift