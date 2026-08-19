#include "arm64_patch.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstring>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {
namespace arm64 {

namespace {

// Shift a 26-bit signed immediate for B/BL.
uint32_t branchImm(uintptr_t from, uintptr_t to) {
    int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return static_cast<uint32_t>((delta >> 2) & 0x03FFFFFF);
}

uint32_t pgr(uintptr_t a) {
    return static_cast<uint32_t>((a >> 12) & 0xFFFFFFFF);
}

}  // namespace

uint32_t encodeB(uintptr_t from, uintptr_t to) {
    return 0x14000000u | branchImm(from, to);
}

uint32_t encodeBl(uintptr_t from, uintptr_t to) {
    return 0x94000000u | branchImm(from, to);
}

uint32_t encodeAdrp(uint32_t rd, uintptr_t pc, uintptr_t target) {
    uint64_t delta = (target & ~0xFFFULL) - (pc & ~0xFFFULL);
    uint64_t imm = static_cast<uint64_t>(static_cast<int64_t>(delta)) >> 12;
    uint32_t immlo = static_cast<uint32_t>(imm & 3);
    uint32_t immhi = static_cast<uint32_t>((imm >> 2) & 0x7FFFF);
    return 0x90000000u | (immlo << 29) | (immhi << 5) | (rd & 31);
}

uint32_t encodeAddImm64(uint32_t rd, uint32_t rn, uint32_t imm) {
    return 0x91000000u | ((imm & 0xFFF) << 10) | ((rn & 31) << 5) | (rd & 31);
}

uint32_t encodeLdrLiteral(uint32_t rd, uintptr_t pc, uintptr_t target) {
    int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(pc);
    int32_t imm19 = static_cast<int32_t>(delta >> 2);
    return 0x58000000u | (static_cast<uint32_t>(imm19 & 0x7FFFF) << 5) | (rd & 31);
}

uint32_t encodeLdrImm(uint32_t rt, uint32_t rn, int32_t imm) {
    uint32_t uimm = static_cast<uint32_t>(imm >> 3) & 0xFFF;
    return 0xF9400000u | (uimm << 10) | ((rn & 31) << 5) | (rt & 31);
}

uint32_t encodeStrImm(uint32_t rt, uint32_t rn, int32_t imm) {
    uint32_t uimm = static_cast<uint32_t>(imm >> 3) & 0xFFF;
    return 0xF9000000u | (uimm << 10) | ((rn & 31) << 5) | (rt & 31);
}

uint32_t encodeMov(uint32_t xd, uint32_t xm) {
    return 0xAA0003E0u | ((xm & 31) << 16) | (xd & 31);
}

bool isBranch(uint32_t insn) {
    return (insn & 0x7C000000) == 0x14000000;
}

bool isAdrp(uint32_t insn) {
    return (insn & 0x9F000000) == 0x90000000;
}

bool isAddImm(uint32_t insn) {
    return (insn & 0xFF800000) == 0x91000000;
}

bool isLdrLiteral(uint32_t insn) {
    return (insn & 0x3B000000) == 0x18000000 &&
           (insn & 0x00800000) == 0x00800000;
}

int64_t decodeAddImm(uint32_t insn) {
    return (insn >> 10) & 0xFFF;
}

bool branchInRange(uintptr_t from, uintptr_t to) {
    int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return delta >= -134217728 && delta <= 134217724;
}

}  // namespace arm64

// ---------------------------------------------------------------------------
// Arm64Patch
// ---------------------------------------------------------------------------

bool Arm64Patch::validatePage(uintptr_t addr) {
    char line[512];
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) == 3) {
            if (addr >= static_cast<uintptr_t>(start) &&
                addr < static_cast<uintptr_t>(end)) {
                if (perms[2] == 'x') found = true;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

bool Arm64Patch::isExecutable(uintptr_t addr) {
    return validatePage(addr);
}

bool Arm64Patch::makeWritable(uintptr_t addr, size_t len) {
    uintptr_t page = addr & ~(static_cast<uintptr_t>(4096) - 1);
    size_t size = ((addr + len) - page + 4095) & ~static_cast<size_t>(4095);
    return mprotect(reinterpret_cast<void*>(page), size,
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

bool Arm64Patch::restoreProtection(uintptr_t addr, size_t len, uintptr_t savedProt) {
    uintptr_t page = addr & ~(static_cast<uintptr_t>(4096) - 1);
    size_t size = ((addr + len) - page + 4095) & ~static_cast<size_t>(4095);
    return mprotect(reinterpret_cast<void*>(page), size,
                    static_cast<int>(savedProt)) == 0;
}

void Arm64Patch::flushCache(uintptr_t addr, size_t len) {
    __builtin___clear_cache(reinterpret_cast<char*>(addr),
                            reinterpret_cast<char*>(addr + len));
}

uint32_t Arm64Patch::readInsn(uintptr_t addr) {
    uint32_t insn = 0;
    memcpy(&insn, reinterpret_cast<const void*>(addr), sizeof(insn));
    return insn;
}

bool Arm64Patch::patchOne(uintptr_t addr, uint32_t word) {
    if (!validatePage(addr)) return false;
    if (!makeWritable(addr, 4)) {
        ARIFT_ERROR(kTagMemory, "makeWritable failed @ %s", utils::hex(addr).c_str());
        return false;
    }
    memcpy(reinterpret_cast<void*>(addr), &word, sizeof(word));
    flushCache(addr, 4);
    return true;
}

bool Arm64Patch::patchWords(uintptr_t addr, const uint32_t* words, size_t count) {
    size_t len = count * 4;
    if (!validatePage(addr)) return false;
    if (!makeWritable(addr, len)) return false;
    memcpy(reinterpret_cast<void*>(addr), words, len);
    flushCache(addr, len);
    return true;
}

bool Arm64Patch::nopSled(uintptr_t addr, size_t count) {
    std::vector<uint32_t> nops(count, arm64::kNop);
    return patchWords(addr, nops.data(), count);
}

bool Arm64Patch::patchBranch(uintptr_t from, uintptr_t to) {
    if (!arm64::branchInRange(from, to)) {
        ARIFT_ERROR(kTagMemory, "Branch out of range: %s -> %s",
                    utils::hex(from).c_str(), utils::hex(to).c_str());
        return false;
    }
    return patchOne(from, arm64::encodeB(from, to));
}

std::vector<uint32_t> Arm64Patch::snapshot(uintptr_t addr, size_t count) {
    std::vector<uint32_t> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(readInsn(addr + i * 4));
    }
    return out;
}

bool Arm64Patch::restore(uintptr_t addr, const std::vector<uint32_t>& snapshot) {
    return patchWords(addr, snapshot.data(), snapshot.size());
}

uintptr_t Arm64Patch::allocateTrampolineNear(uintptr_t target, size_t size,
                                             int64_t maxDelta) {
    // Search for a free executable mapping within maxDelta of target.
    uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    uintptr_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t prevEnd = 0;
    uintptr_t candidate = 0;
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3) continue;
        uintptr_t gapStart = prevEnd;
        uintptr_t gapEnd = static_cast<uintptr_t>(start);
        if (gapStart != 0 && gapEnd > gapStart) {
            int64_t delta = static_cast<int64_t>(gapStart) -
                            static_cast<int64_t>(target);
            if (delta > -maxDelta && delta < maxDelta) {
                candidate = (gapStart + pageSize - 1) & ~(pageSize - 1);
                if (candidate + alignedSize <= gapEnd) {
                    found = true;
                    break;
                }
            }
        }
        prevEnd = static_cast<uintptr_t>(end);
    }
    fclose(f);
    if (!found) return 0;

    void* mapped = mmap(reinterpret_cast<void*>(candidate), alignedSize,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mapped == MAP_FAILED) return 0;
    return reinterpret_cast<uintptr_t>(mapped);
}

size_t Arm64Patch::relocateInstructions(uintptr_t src, uintptr_t dst, size_t count,
                                        uintptr_t* outEndAddr) {
    // Copy raw instructions; fix unconditional branches by re-encoding.
    size_t copied = 0;
    for (size_t i = 0; i < count; ++i) {
        uintptr_t from = src + i * 4;
        uint32_t insn = readInsn(from);
        if (arm64::isBranch(insn) &&
            (insn & 0xFC000000) == 0x14000000) {
            int64_t delta = static_cast<int64_t>(static_cast<int32_t>(
                (insn & 0x03FFFFFF) << 2) >> 2);
            uintptr_t target = from + delta;
            if (!arm64::branchInRange(dst + i * 4, target)) {
                ARIFT_ERROR(kTagMemory, "Cannot relocate branch at offset %zu", i);
                return 0;
            }
            insn = arm64::encodeB(dst + i * 4, target);
        }
        patchOne(dst + i * 4, insn);
        ++copied;
    }
    if (outEndAddr) *outEndAddr = dst + copied * 4;
    return copied;
}

}  // namespace arift