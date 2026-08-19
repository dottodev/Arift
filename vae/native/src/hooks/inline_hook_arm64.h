#pragma once

#include <cstdint>
#include <functional>

namespace arift {

// ARM64 inline hook: overwrites the first instructions of `target` with a
// branch to `replacement`; original code is relocated to a trampoline so
// callers can still reach the real function.
class InlineHookArm64 {
public:
    struct Config {
        size_t maxTrampolineBytes;
        bool verifyExecutable;
        bool relocateBranches;
        Config() : maxTrampolineBytes(64), verifyExecutable(true), relocateBranches(true) {}
    };

    // Install. Returns true on success. `outTrampoline` receives the
    // trampoline address (call it to reach the original function).
    static bool install(uintptr_t target, uintptr_t replacement,
                        uintptr_t* outTrampoline = nullptr,
                        const Config& cfg = Config());

    // Uninstall: restore original instructions from the engine record.
    static bool uninstall(uintptr_t target);

    // Restore + reinstall (used after trampoline cache invalidation).
    static bool reinstall(uintptr_t target);

    // Test-only: check how many instructions would be relocated.
    static size_t analyze(uintptr_t target, const Config& cfg = Config());

private:
    static size_t countInstructionsToPatch(uintptr_t target,
                                           uintptr_t trampoline,
                                           size_t maxBytes);
};

}  // namespace arift