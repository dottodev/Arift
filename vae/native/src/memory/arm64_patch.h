#pragma once

#include <cstdint>
#include <vector>

namespace arift {

// ARM64 instruction encoding + patching helpers.
namespace arm64 {

// Instruction encoding constants.
constexpr uint32_t kNop = 0xD503201F;
constexpr uint32_t kBrk = 0xD4200000;
constexpr uint32_t kRet = 0xD65F03C0;

// Encode a B (unconditional branch, +-128MB) from `from` to `to`.
uint32_t encodeB(uintptr_t from, uintptr_t to);

// Encode a BL (branch-and-link).
uint32_t encodeBl(uintptr_t from, uintptr_t to);

// Encode an ADRP instruction: register `rd`, PC-relative page address.
uint32_t encodeAdrp(uint32_t rd, uintptr_t pc, uintptr_t target);

// Encode ADD immediate (64-bit, rd = rn + imm, imm 0..4095).
uint32_t encodeAddImm64(uint32_t rd, uint32_t rn, uint32_t imm);

// Encode LDR (literal, 64-bit): rd = *(uint64_t*)pc_relative.
uint32_t encodeLdrLiteral(uint32_t rd, uintptr_t pc, uintptr_t target);

// Encode LDR x0, [x1, #imm]
uint32_t encodeLdrImm(uint32_t rt, uint32_t rn, int32_t imm);

// Encode STR x0, [x1, #imm]
uint32_t encodeStrImm(uint32_t rt, uint32_t rn, int32_t imm);

// Encode MOV xd, xm (alias of ORR).
uint32_t encodeMov(uint32_t xd, uint32_t xm);

// Decode helpers
bool isBranch(uint32_t insn);
bool isAdrp(uint32_t insn);
bool isAddImm(uint32_t insn);
bool isLdrLiteral(uint32_t insn);

int64_t decodeAddImm(uint32_t insn);

// Compute a branch range check.
bool branchInRange(uintptr_t from, uintptr_t to);

}  // namespace arm64

// In-place patch: writes instruction words with cache maintenance.
class Arm64Patch {
public:
    // Patch `count` instructions starting at `addr` with the given words.
    static bool patchWords(uintptr_t addr, const uint32_t* words, size_t count);

    // Write a single instruction.
    static bool patchOne(uintptr_t addr, uint32_t word);

    // Patch a NOP sled.
    static bool nopSled(uintptr_t addr, size_t count);

    // Patch a B to target.
    static bool patchBranch(uintptr_t from, uintptr_t to);

    // Read the current instruction at addr.
    static uint32_t readInsn(uintptr_t addr);

    // Flush instruction cache for the given range.
    static void flushCache(uintptr_t addr, size_t len);

    // Validate that `addr` is in executable memory (via /proc/self/maps).
    static bool isExecutable(uintptr_t addr);

    // Write-protect toggle for a page range.
    static bool makeWritable(uintptr_t addr, size_t len);
    static bool restoreProtection(uintptr_t addr, size_t len, uintptr_t savedProt);

    // Snapshot the original instructions (for restore/unhook).
    static std::vector<uint32_t> snapshot(uintptr_t addr, size_t count);

    // Restore previously snapshotted instructions.
    static bool restore(uintptr_t addr, const std::vector<uint32_t>& snapshot);

    // Trampoline builder: allocate executable memory near `target`.
    static uintptr_t allocateTrampolineNear(uintptr_t target, size_t size,
                                            int64_t maxDelta = 120 * 1024 * 1024);

    // Copy instructions into a trampoline, fixing PC-relative branches.
    // Returns the number of instructions copied or 0 on failure.
    static size_t relocateInstructions(uintptr_t src, uintptr_t dst, size_t count,
                                       uintptr_t* outEndAddr);

private:
    static bool validatePage(uintptr_t addr);
};

}  // namespace arift