#include "inline_hook_arm64.h"

#include <cstring>

#include "arift_log.h"
#include "arift_utils.h"
#include "arm64_patch.h"
#include "hook_engine.h"

namespace arift {

namespace {

bool patchInto(uintptr_t target, uintptr_t replacement,
               uintptr_t trampoline, size_t insnsToPatch,
               std::vector<uint32_t>& original) {
    // 1. Relocate original instructions into the trampoline.
    size_t copied = Arm64Patch::relocateInstructions(
        target, trampoline, insnsToPatch, nullptr);
    if (copied != insnsToPatch) {
        ARIFT_ERROR(kTagHooks, "Relocation failed: copied=%zu want=%zu",
                    copied, insnsToPatch);
        return false;
    }

    // 2. Write the final branch in the trampoline back to target+offset.
    uintptr_t after = target + insnsToPatch * 4;
    if (!Arm64Patch::patchBranch(trampoline + copied * 4, after)) {
        return false;
    }

    // 3. Overwrite the target prologue with a branch to replacement.
    std::vector<uint32_t> words;
    words.reserve(insnsToPatch + 1);
    for (size_t i = 0; i < insnsToPatch; ++i) {
        words.push_back(arm64::kNop);
    }
    words[0] = arm64::encodeB(target, replacement);
    if (!Arm64Patch::patchWords(target, words.data(), words.size())) {
        return false;
    }

    original = Arm64Patch::snapshot(target, insnsToPatch);
    return true;
}

}  // namespace

size_t InlineHookArm64::countInstructionsToPatch(uintptr_t target,
                                                 uintptr_t trampoline,
                                                 size_t maxBytes) {
    size_t insns = 0;
    size_t bytes = 0;
    while (bytes + 4 <= maxBytes) {
        uint32_t insn = Arm64Patch::readInsn(target + bytes);
        if (arm64::isBranch(insn)) break;   // patch before any branch
        insns++;
        bytes += 4;
        if (bytes >= 16) break;             // 4 instructions is plenty
    }
    if (insns == 0) insns = 1;
    return insns;
}

size_t InlineHookArm64::analyze(uintptr_t target, const Config& cfg) {
    return countInstructionsToPatch(target, target, cfg.maxTrampolineBytes);
}

bool InlineHookArm64::install(uintptr_t target, uintptr_t replacement,
                              uintptr_t* outTrampoline, const Config& cfg) {
    if (target == 0 || replacement == 0 || target == replacement) {
        ARIFT_ERROR(kTagHooks, "Bad install args");
        return false;
    }
    if (HookEngine::instance().find(target)) {
        ARIFT_WARN(kTagHooks, "Already hooked @ %s", utils::hex(target).c_str());
        return false;
    }
    if (cfg.verifyExecutable && !Arm64Patch::isExecutable(target)) {
        ARIFT_ERROR(kTagHooks, "Target not executable @ %s", utils::hex(target).c_str());
        return false;
    }

    uintptr_t trampoline = Arm64Patch::allocateTrampolineNear(
        target, cfg.maxTrampolineBytes + 64);
    if (trampoline == 0) {
        ARIFT_ERROR(kTagHooks, "No trampoline space near %s", utils::hex(target).c_str());
        return false;
    }

    size_t insns = countInstructionsToPatch(target, trampoline,
                                            cfg.maxTrampolineBytes);

    std::vector<uint32_t> original;
    if (!patchInto(target, replacement, trampoline, insns, original)) {
        return false;
    }

    HookRecord rec;
    rec.type = HookType::kInline;
    rec.name = "inline:" + utils::hex(target);
    rec.target = target;
    rec.replacement = replacement;
    rec.trampoline = trampoline;
    rec.originalBytes = original;
    rec.active = true;
    HookEngine::instance().registerHook(rec);

    if (outTrampoline) *outTrampoline = trampoline;

    ARIFT_DEBUG(kTagHooks, "Installed inline hook @ %s -> %s (tramp %s, %zu insns)",
                utils::hex(target).c_str(), utils::hex(replacement).c_str(),
                utils::hex(trampoline).c_str(), insns);
    return true;
}

bool InlineHookArm64::uninstall(uintptr_t target) {
    HookRecord* rec = HookEngine::instance().find(target);
    if (!rec || rec->type != HookType::kInline) return false;
    bool ok = Arm64Patch::restore(target, rec->originalBytes);
    if (ok) {
        HookEngine::instance().unregisterHook(target);
    }
    return ok;
}

bool InlineHookArm64::reinstall(uintptr_t target) {
    HookRecord* rec = HookEngine::instance().find(target);
    if (!rec) return false;
    return install(rec->target, rec->replacement, nullptr);
}

}  // namespace arift