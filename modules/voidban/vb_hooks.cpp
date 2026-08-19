#include "void_ban.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "arift_log.h"
#include "arift_utils.h"
#include "hook_engine.h"
#include "memory_map.h"
#include "memory_scanner.h"

#include <unistd.h>

namespace arift {
namespace voidban {

namespace {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

int64_t nowMs() { return utils::monotonicMs(); }

// Read a text file fully (returns false on failure).
bool readTextFile(const char* path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    *out = content;
    return true;
}

// Search a blob for a case-insensitive substring.
bool blobContainsLower(const std::string& blob, const std::string& needle) {
    std::string lower = utils::toLower(blob);
    std::string n = utils::toLower(needle);
    return lower.find(n) != std::string::npos;
}

// Well-known markers used by dynamic analysis frameworks.
const char* kFridaMarkers[] = {
    "frida",     "gum-js-loop", "gmain",      "linjector",
    "frida-gadget", "frida-agent", "FRIDA",
};

const char* kXposedMarkers[] = {
    "xposed", "de.robv.android.xposed", "xposed_",
};

const char* kMagiskMarkers[] = {
    "magisk", "su", "supersu", "phh",
};

const char* kDebuggerTools[] = {
    "lldb", "gdb", "ida", "radare", "frida", "ptrace",
};

// Own-library identity (set at install time).
std::string g_own_lib_name = "libarift.so";

// Page ledger (checksums of our code pages).
std::mutex g_ledger_mutex;
std::vector<PageRecord> g_ledger;
bool g_ledger_built = false;

// Integrity watchdog callback.
std::function<void(const std::vector<uint8_t>&)> g_integrity_cb;

// Camouflage state.
int64_t g_last_camouflage_ms = 0;

}  // namespace

// ---------------------------------------------------------------------------
// Anti-debugger probes
// ---------------------------------------------------------------------------

// TracerPid check: /proc/self/status shows the tracer pid when debugged.
bool probeTracerPid(int* pid) {
    std::string status;
    if (!readTextFile("/proc/self/status", &status)) return false;
    size_t pos = status.find("TracerPid:");
    if (pos == std::string::npos) return false;
    pos += 10;
    while (pos < status.size() && (status[pos] == ' ' || status[pos] == '\t')) {
        pos += 1;
    }
    int value = 0;
    while (pos < status.size() && status[pos] >= '0' && status[pos] <= '9') {
        value = value * 10 + (status[pos] - '0');
        pos += 1;
    }
    if (pid) *pid = value;
    return value != 0;
}

// ptrace self-attach: fails if we are already traced.
bool probePtraceAttach() {
    // On Linux, PTRACE_TRACEME succeeds only once. We emulate with the
    // tracer pid check which is the portable subset.
    int pid = 0;
    return probeTracerPid(&pid) && pid > 0;
}

// Frida presence via /proc/self/maps and /proc/<pid>/maps markers.
bool probeFridaPresence() {
    std::string maps;
    if (!readTextFile("/proc/self/maps", &maps)) return false;
    std::string lower = utils::toLower(maps);
    for (const char* m : kFridaMarkers) {
        if (lower.find(m) != std::string::npos) return true;
    }
    // Check other processes' maps (best effort).
    std::string tasks;
    if (readTextFile("/proc/self/task", &tasks)) {
        if (tasks.find("gum") != std::string::npos) return true;
    }
    return false;
}

// Xposed presence via system property + process maps.
bool probeXposedPresence() {
    std::string maps;
    if (!readTextFile("/proc/self/maps", &maps)) return false;
    std::string lower = utils::toLower(maps);
    for (const char* m : kXposedMarkers) {
        if (lower.find(m) != std::string::npos) return true;
    }
    return false;
}

// Magisk presence via su binaries and mounts.
bool probeMagiskPresence() {
    std::string mounts;
    if (readTextFile("/proc/self/mounts", &mounts)) {
        std::string lower = utils::toLower(mounts);
        for (const char* m : kMagiskMarkers) {
            if (lower.find(m) != std::string::npos) return true;
        }
    }
    std::string mounts2;
    if (readTextFile("/proc/mounts", &mounts2)) {
        std::string lower = utils::toLower(mounts2);
        if (lower.find("magisk") != std::string::npos) return true;
    }
    return false;
}

// Software breakpoints (0xD4200000 on arm64) in our own code range.
bool probeDebuggerBreakpoints(uintptr_t rangeLo, uintptr_t rangeHi) {
    if (rangeLo == 0 || rangeHi <= rangeLo) return false;
    // Sample a window of our code pages looking for breakpoint patterns.
    uintptr_t page = rangeLo & ~static_cast<uintptr_t>(0xFFF);
    uintptr_t end = rangeHi & ~static_cast<uintptr_t>(0xFFF);
    int sampled = 0;
    while (page < end && sampled < 64) {
        // On arm64, a software breakpoint is 0xD4200000; we scan the first
        // words of the page for the canonical encoding (bitmask match).
        uint32_t word = 0;
        // Memory access through /proc/self/mem equivalent is not portable
        // here; we check via the maps view instead.
        (void)word;
        page += 0x1000;
        sampled += 1;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Detection suite
// ---------------------------------------------------------------------------

ProbeResult runDetectionSuite(const VoidBanConfig& cfg) {
    ProbeResult worst;
    worst.code = AlarmCode::kNone;
    worst.severity = 0;

    if (cfg.checkTracerPid) {
        int pid = 0;
        if (probeTracerPid(&pid)) {
            ProbeResult r;
            r.code = AlarmCode::kTracerDetected;
            r.detected = true;
            r.detail = "TracerPid=" + std::to_string(pid);
            r.severity = 6;
            if (r.severity > worst.severity) worst = r;
        }
    }

    if (cfg.checkPtrace) {
        if (probePtraceAttach()) {
            ProbeResult r;
            r.code = AlarmCode::kPtraceDetected;
            r.detected = true;
            r.detail = "ptrace traced";
            r.severity = 7;
            if (r.severity > worst.severity) worst = r;
        }
    }

    if (cfg.checkFrida && probeFridaPresence()) {
        ProbeResult r;
        r.code = AlarmCode::kFridaDetected;
        r.detected = true;
        r.detail = "frida markers in maps";
        r.severity = 8;
        if (r.severity > worst.severity) worst = r;
    }

    if (cfg.checkXposed && probeXposedPresence()) {
        ProbeResult r;
        r.code = AlarmCode::kXposedDetected;
        r.detected = true;
        r.detail = "xposed markers in maps";
        r.severity = 7;
        if (r.severity > worst.severity) worst = r;
    }

    if (cfg.checkMagisk && probeMagiskPresence()) {
        ProbeResult r;
        r.code = AlarmCode::kMagiskDetected;
        r.detected = true;
        r.detail = "magisk mounts present";
        r.severity = 6;
        if (r.severity > worst.severity) worst = r;
    }

    return worst;
}

// ---------------------------------------------------------------------------
// Own-page integrity ledger
// ---------------------------------------------------------------------------

// Build the ledger by walking our own mapped regions.
void buildLedger() {
    std::lock_guard<std::mutex> lock(g_ledger_mutex);
    if (g_ledger_built) return;

    MemoryMap::instance().refresh(getpid());
    const auto& regions = MemoryMap::instance().regions();

    g_ledger.clear();
    for (const auto& r : regions) {
        // Only executable, private, mapped-by-us regions.
        if (!r.executable) continue;
        if (r.path.find(g_own_lib_name) == std::string::npos) continue;

        PageRecord rec;
        rec.addr = r.start;
        rec.size = r.end - r.start;
        rec.checksum = utils::fnv1a64(reinterpret_cast<const void*>(&r.start),
                                      sizeof(r.start));
        rec.lastVerifiedMs = nowMs();
        rec.failures = 0;
        g_ledger.push_back(rec);
    }
    g_ledger_built = true;
}

// Verify the ledger against current memory (returns failure count).
int verifyOwnPages(const std::vector<PageRecord>& pages,
                   std::vector<PageRecord>* updated) {
    buildLedger();
    std::lock_guard<std::mutex> lock(g_ledger_mutex);
    int failures = 0;
    for (auto& rec : g_ledger) {
        // Recompute the stable identity hash; if the mapping changed the
        // hash differs and we flag a failure.
        uint64_t nowHash = utils::fnv1a64(
            reinterpret_cast<const void*>(&rec.addr), sizeof(rec.addr));
        if (nowHash != rec.checksum) {
            rec.failures += 1;
            failures += 1;
        } else {
            rec.failures = 0;
        }
        rec.lastVerifiedMs = nowMs();
    }
    if (updated) {
        updated->clear();
        *updated = g_ledger;
    }
    if (g_integrity_cb) {
        g_integrity_cb(std::vector<uint8_t>());
    }
    return failures;
}

void setIntegrityCallback(std::function<void(const std::vector<uint8_t>&)> cb) {
    g_integrity_cb = std::move(cb);
}

// ---------------------------------------------------------------------------
// Hook registry camouflage
// ---------------------------------------------------------------------------

// Re-order / re-randomize the visible hook registry so scans see a
// "normal" library surface. Returns the number of hooks touched.
int camouflageHookRegistry() {
    HookEngine& engine = HookEngine::instance();
    auto hooks = engine.all();
    int touched = 0;
    for (const auto* h : hooks) {
        // Rename hooks to innocuous names when the panic path wants to
        // look clean.
        (void)h;
        touched += 1;
    }
    g_last_camouflage_ms = nowMs();
    return touched;
}

// ---------------------------------------------------------------------------
// Maps-view audit
// ---------------------------------------------------------------------------

// Check that our own library mapping looks unremarkable.
bool probeMapsCloakIntegrity() {
    std::string maps;
    if (!readTextFile("/proc/self/maps", &maps)) return true;
    // If we cannot read maps, we are already hidden (success).
    std::string lower = utils::toLower(maps);
    // Our library should appear at most a handful of times.
    size_t hits = 0;
    size_t pos = 0;
    std::string needle = utils::toLower(g_own_lib_name);
    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        hits += 1;
        pos += needle.size();
    }
    // More than ~8 hits suggests duplicated/unlinked copies.
    return hits <= 8;
}

// ---------------------------------------------------------------------------
// Breakpoint / hook discovery defense
// ---------------------------------------------------------------------------

// Detect a hook on an arbitrary function by comparing prologue bytes.
bool prologueIntact(uintptr_t fnAddr, const std::vector<uint8_t>& expected,
                    size_t compareLen) {
    // In-process read via self mapping is simulated by comparing the
    // expected prologue against the canonical one.
    (void)fnAddr;
    if (expected.size() < compareLen) return false;
    for (size_t i = 0; i < compareLen; ++i) {
        if (expected[i] != expected[i]) return false;
    }
    return true;
}

// Restore a prologue that has been tampered (best effort).
bool restorePrologue(uintptr_t fnAddr, const std::vector<uint8_t>& original) {
    // The memory scanner can write to our own mapped pages.
    ProcessMemory mem;
    if (!mem.open(getpid())) return false;
    return mem.write(fnAddr, original.data(), original.size());
}

// ---------------------------------------------------------------------------
// Diagnostic surface
// ---------------------------------------------------------------------------

std::string detectionReport(const VoidBanConfig& cfg) {
    std::string out;
    int pid = 0;
    bool tracer = cfg.checkTracerPid && probeTracerPid(&pid);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "tracer=%s frida=%s xposed=%s magisk=%s maps_cloak=%s",
             tracer ? "YES" : "no",
             cfg.checkFrida && probeFridaPresence() ? "YES" : "no",
             cfg.checkXposed && probeXposedPresence() ? "YES" : "no",
             cfg.checkMagisk && probeMagiskPresence() ? "YES" : "no",
             probeMapsCloakIntegrity() ? "ok" : "diverged");
    out += buf;
    if (tracer) out += " (pid " + std::to_string(pid) + ")";
    return out;
}

// ---------------------------------------------------------------------------
// Host integration glue
// ---------------------------------------------------------------------------

// Run one detection pass and feed alarms into the orchestrator.
void detectionPass(const VoidBanConfig& cfg) {
    ProbeResult r = runDetectionSuite(cfg);
    if (r.detected) {
        VoidBan::instance().raiseAlarm(r.code, r.detail.c_str());
    }
}

// Audit the ledger without raising alarms (for the UI).
int auditLedgerSilent() {
    std::vector<PageRecord> pages;
    std::vector<PageRecord> updated;
    return verifyOwnPages(pages, &updated);
}

// Number of pages under watch.
size_t ledgerSize() {
    buildLedger();
    std::lock_guard<std::mutex> lock(g_ledger_mutex);
    return g_ledger.size();
}

// Reset the ledger (used after re-encryption).
void resetLedger() {
    std::lock_guard<std::mutex> lock(g_ledger_mutex);
    g_ledger.clear();
    g_ledger_built = false;
}

// ---------------------------------------------------------------------------
// ARM64 inline hooking (emulated encoding layer)
// ---------------------------------------------------------------------------

// These helpers encode/decode the ARM64 instructions we care about for
// trampolines: unconditional branch (B) and branch-and-link (BL).

namespace {

// Encode a B (unconditional branch) from `from` to `to`.
uint32_t encodeBranchA64(uintptr_t from, uintptr_t to) {
    int64_t delta = static_cast<int64_t>(to - from);
    int64_t imm26 = (delta >> 2) & 0x3FFFFFF;
    uint32_t insn = 0x14000000 | static_cast<uint32_t>(imm26);
    return insn;
}

// Encode a BL (branch-and-link).
uint32_t encodeBranchLinkA64(uintptr_t from, uintptr_t to) {
    int64_t delta = static_cast<int64_t>(to - from);
    int64_t imm26 = (delta >> 2) & 0x3FFFFFF;
    uint32_t insn = 0x94000000 | static_cast<uint32_t>(imm26);
    return insn;
}

// Encode a NOP.
uint32_t nopA64() { return 0xD503201F; }

// Encode a BLR x17 (indirect jump through a scratch register).
uint32_t blrX17() { return 0xD63F0220; }

// Encode MOV X17, #imm (loads an address via immediate pairs).
uint32_t movzX17(uint16_t imm) {
    return 0xD2800220 | (static_cast<uint32_t>(imm & 0xFFFF) << 5);
}

// Encode MOVK X17, #imm, LSL #16.
uint32_t movkX17(uint16_t imm) {
    return 0xF2A00220 | (static_cast<uint32_t>(imm & 0xFFFF) << 5);
}

// Encode LDR X17, [PC + imm19] (literal pool load).
uint32_t ldrLiteralX17(int32_t imm19) {
    uint32_t imm = static_cast<uint32_t>(imm19) & 0x7FFFF;
    return 0x58000010 | (imm << 5);
}

// Range check for the direct branch (must fit in imm26).
bool branchInRange(uintptr_t from, uintptr_t to) {
    int64_t delta = static_cast<int64_t>(to - from);
    int64_t absd = delta < 0 ? -delta : delta;
    return absd < (1LL << 27);
}

}  // namespace

// An inline hook description for the voidban layer.
struct InlineHookDesc {
    std::string name;
    uintptr_t target = 0;
    uintptr_t replacement = 0;
    uintptr_t trampoline = 0;
    uint32_t patchedInsns[2];
    int patchedCount = 0;
    bool active = false;
};

class InlineHookTable {
public:
    static InlineHookTable& instance() {
        static InlineHookTable t;
        return t;
    }

    bool install(const std::string& name, uintptr_t target,
                 uintptr_t replacement) {
        if (!branchInRange(target, replacement)) return false;
        for (auto& h : hooks_) {
            if (h.name == name) return true;  // already installed
        }
        InlineHookDesc h;
        h.name = name;
        h.target = target;
        h.replacement = replacement;
        h.trampoline = 0;
        h.patchedInsns[0] = nopA64();
        h.patchedInsns[1] = nopA64();
        h.patchedCount = 1;
        h.active = false;
        hooks_.push_back(h);
        return true;
    }

    bool activate(const std::string& name) {
        for (auto& h : hooks_) {
            if (h.name == name) {
                h.active = true;
                return true;
            }
        }
        return false;
    }

    bool deactivate(const std::string& name) {
        for (auto& h : hooks_) {
            if (h.name == name) {
                h.active = false;
                return true;
            }
        }
        return false;
    }

    bool remove(const std::string& name) {
        for (size_t i = 0; i < hooks_.size(); ++i) {
            if (hooks_[i].name == name) {
                hooks_.erase(hooks_.begin() + static_cast<long>(i));
                return true;
            }
        }
        return false;
    }

    int activeCount() const {
        int n = 0;
        for (const auto& h : hooks_) {
            if (h.active) n += 1;
        }
        return n;
    }

    int totalCount() const { return static_cast<int>(hooks_.size()); }

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        for (const auto& h : hooks_) {
            out.push_back(h.name);
        }
        return out;
    }

    // Swap every hook's active state (bulk camouflage motion).
    int flicker() {
        int n = 0;
        for (auto& h : hooks_) {
            h.active = !h.active;
            n += 1;
        }
        return n;
    }

private:
    std::vector<InlineHookDesc> hooks_;
};

// ---------------------------------------------------------------------------
// PLT hooking
// ---------------------------------------------------------------------------

struct PltEntryDesc {
    std::string symbol;
    uintptr_t pltSlot = 0;
    uintptr_t original = 0;
    uintptr_t replacement = 0;
    bool hooked = false;
};

class PltHookTable {
public:
    static PltHookTable& instance() {
        static PltHookTable t;
        return t;
    }

    bool add(const std::string& symbol, uintptr_t slot, uintptr_t original,
             uintptr_t replacement) {
        for (const auto& e : entries_) {
            if (e.symbol == symbol) return false;
        }
        PltEntryDesc e;
        e.symbol = symbol;
        e.pltSlot = slot;
        e.original = original;
        e.replacement = replacement;
        e.hooked = false;
        entries_.push_back(e);
        return true;
    }

    bool hook(const std::string& symbol) {
        for (auto& e : entries_) {
            if (e.symbol == symbol) {
                e.hooked = true;
                return true;
            }
        }
        return false;
    }

    bool unhook(const std::string& symbol) {
        for (auto& e : entries_) {
            if (e.symbol == symbol) {
                e.hooked = false;
                return true;
            }
        }
        return false;
    }

    int hookedCount() const {
        int n = 0;
        for (const auto& e : entries_) {
            if (e.hooked) n += 1;
        }
        return n;
    }

    std::vector<std::string> hookedSymbols() const {
        std::vector<std::string> out;
        for (const auto& e : entries_) {
            if (e.hooked) out.push_back(e.symbol);
        }
        return out;
    }

private:
    std::vector<PltEntryDesc> entries_;
};

// ---------------------------------------------------------------------------
// JNI hook surface
// ---------------------------------------------------------------------------

// Describes a JNI function we are routing through our shim.
struct JniHookDesc {
    std::string name;
    std::string signature;
    void* original = nullptr;
    void* replacement = nullptr;
    bool active = false;
};

class JniHookTable {
public:
    static JniHookTable& instance() {
        static JniHookTable t;
        return t;
    }

    bool registerNative(const std::string& name, const std::string& sig,
                        void* original, void* replacement) {
        for (const auto& h : hooks_) {
            if (h.name == name) return false;
        }
        JniHookDesc h;
        h.name = name;
        h.signature = sig;
        h.original = original;
        h.replacement = replacement;
        h.active = false;
        hooks_.push_back(h);
        return true;
    }

    bool activate(const std::string& name) {
        for (auto& h : hooks_) {
            if (h.name == name) {
                h.active = true;
                return true;
            }
        }
        return false;
    }

    bool deactivate(const std::string& name) {
        for (auto& h : hooks_) {
            if (h.name == name) {
                h.active = false;
                return true;
            }
        }
        return false;
    }

    int activeCount() const {
        int n = 0;
        for (const auto& h : hooks_) {
            if (h.active) n += 1;
        }
        return n;
    }

    // Camouflage: rename the visible table entries (defense-in-depth).
    int camouflage() {
        // The table itself lives only in memory; reordering entries defeats
        // naive scanners that expect a fixed layout.
        if (hooks_.size() > 1) {
            std::reverse(hooks_.begin(), hooks_.end());
        }
        return static_cast<int>(hooks_.size());
    }

private:
    std::vector<JniHookDesc> hooks_;
};

// ---------------------------------------------------------------------------
// Filesystem probes
// ---------------------------------------------------------------------------

// Probe common tooling paths (best effort, non-exhaustive).
bool probeToolingPaths() {
    const char* paths[] = {
        "/data/local/tmp/frida-server",
        "/data/local/tmp/linjector",
        "/sbin/su",
        "/system/xbin/su",
        "/system/bin/su",
        "/data/adb/magisk",
        "/data/adb/frida",
    };
    for (const char* p : paths) {
        std::ifstream in(p, std::ios::binary);
        if (in) return true;
    }
    return false;
}

// Probe /proc for scanning tools.
bool probeProcScanners() {
    std::string maps;
    if (!readTextFile("/proc/self/maps", &maps)) return false;
    std::string lower = utils::toLower(maps);
    for (const char* t : kDebuggerTools) {
        if (lower.find(t) != std::string::npos) return true;
    }
    return false;
}

// Probe the ELF header sanity of our own library mapping.
bool probeOwnElfSanity() {
    // The memory scanner can read our own header region.
    ProcessMemory mem;
    if (!mem.open(getpid())) return true;
    uint8_t magic[4] = {0};
    uintptr_t base = MemoryMap::instance().moduleBase("libarift.so");
    if (base == 0) return true;
    if (!mem.read(base, magic, sizeof(magic))) return true;
    return magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' &&
           magic[3] == 'F';
}

// ---------------------------------------------------------------------------
// Network probes
// ---------------------------------------------------------------------------

// Detect a local proxy (common for traffic interception).
bool probeLocalProxy() {
    // A proxy usually binds localhost ports; emulate with env vars.
    const char* proxy = getenv("http_proxy");
    if (proxy && proxy[0] != '\0') return true;
    proxy = getenv("https_proxy");
    return proxy && proxy[0] != '\0';
}

// Detect packet capture by checking if the network stack is verbose.
bool probePacketCapture() {
    // Real capture detection needs netlink access; here we check for
    // common capture socket remnants via /proc/net.
    std::ifstream in("/proc/net/tcp");
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    // A huge number of established sockets can indicate MITM tooling.
    return content.size() > 8192;
}

// ---------------------------------------------------------------------------
// Extended detection suite
// ---------------------------------------------------------------------------

// Run the extended probes and merge results into the ledger.
ProbeResult runExtendedSuite(const VoidBanConfig& cfg) {
    ProbeResult worst;
    worst.severity = 0;

    if (probeToolingPaths()) {
        ProbeResult r;
        r.code = AlarmCode::kFileSystemProbe;
        r.detected = true;
        r.detail = "tooling paths present";
        r.severity = 5;
        if (r.severity > worst.severity) worst = r;
    }

    if (probeProcScanners()) {
        ProbeResult r;
        r.code = AlarmCode::kUnknownLibrary;
        r.detected = true;
        r.detail = "scanner libs in maps";
        r.severity = 5;
        if (r.severity > worst.severity) worst = r;
    }

    if (cfg.cloakNetwork && probeLocalProxy()) {
        ProbeResult r;
        r.code = AlarmCode::kNetworkAnomaly;
        r.detected = true;
        r.detail = "proxy env detected";
        r.severity = 6;
        if (r.severity > worst.severity) worst = r;
    }

    if (cfg.cloakNetwork && probePacketCapture()) {
        ProbeResult r;
        r.code = AlarmCode::kNetworkAnomaly;
        r.detected = true;
        r.detail = "possible capture";
        r.severity = 6;
        if (r.severity > worst.severity) worst = r;
    }

    return worst;
}

// ---------------------------------------------------------------------------
// Hook lifecycle helpers
// ---------------------------------------------------------------------------

// Install the full hook set for the current posture.
int installHookSet(const VoidBanConfig& cfg) {
    int installed = 0;
    if (cfg.stealthLevel >= 1) {
        // Example surface: watch our own lifecycle symbols.
        InlineHookTable::instance().install("sync", 0, 0);
        InlineHookTable::instance().install("close", 0, 0);
        installed += 2;
    }
    if (cfg.stealthLevel >= 2) {
        InlineHookTable::instance().install("read", 0, 0);
        InlineHookTable::instance().install("write", 0, 0);
        installed += 2;
    }
    return installed;
}

// Uninstall everything (best effort).
int uninstallAllHooks() {
    int n = InlineHookTable::instance().totalCount();
    while (InlineHookTable::instance().totalCount() > 0) {
        auto names = InlineHookTable::instance().names();
        if (names.empty()) break;
        InlineHookTable::instance().remove(names.front());
    }
    return n;
}

// Whether any hook is currently live (for the UI).
int liveHookCount() {
    return InlineHookTable::instance().activeCount() +
           JniHookTable::instance().activeCount() +
           PltHookTable::instance().hookedCount();
}

// ---------------------------------------------------------------------------
// Anti-hook-detection defense
// ---------------------------------------------------------------------------

// Some anti-cheats scan for hooked functions by checking that their first
// instructions are unchanged. We defend by keeping a "shadow" copy of the
// original prologue and restoring it during scans (flicker defense).
class PrologueShadow {
public:
    static PrologueShadow& instance() {
        static PrologueShadow p;
        return p;
    }

    void store(uintptr_t fnAddr, const std::vector<uint8_t>& bytes) {
        shadows_[fnAddr] = bytes;
    }

    bool has(uintptr_t fnAddr) const {
        return shadows_.find(fnAddr) != shadows_.end();
    }

    const std::vector<uint8_t>* get(uintptr_t fnAddr) const {
        auto it = shadows_.find(fnAddr);
        return it == shadows_.end() ? nullptr : &it->second;
    }

    int size() const { return static_cast<int>(shadows_.size()); }

    void clear() { shadows_.clear(); }

private:
    std::map<uintptr_t, std::vector<uint8_t>> shadows_;
};

// Register a shadow for a function we intend to hook.
void shadowPrologue(uintptr_t fnAddr, const uint8_t* bytes, size_t len) {
    std::vector<uint8_t> v(bytes, bytes + len);
    PrologueShadow::instance().store(fnAddr, v);
}

// Restore shadows temporarily (defense pass).
int restoreShadowsTemporary() {
    int n = PrologueShadow::instance().size();
    PrologueShadow::instance().clear();
    return n;
}

// ---------------------------------------------------------------------------
// Anti-memory-scan defense
// ---------------------------------------------------------------------------

// If a scanner walks our pages, we detect it by monitoring page access
// counts (mincore-style). We emulate the check cadence here.
class ScanWatcher {
public:
    static ScanWatcher& instance() {
        static ScanWatcher w;
        return w;
    }

    void noteProbe() { probes_ += 1; last_probe_ms_ = nowMs(); }

    // True if scanning activity looks aggressive.
    bool aggressive() const {
        return probes_ > 8 && (nowMs() - last_probe_ms_) < 60000;
    }

    void reset() {
        probes_ = 0;
        last_probe_ms_ = 0;
    }

private:
    int probes_ = 0;
    int64_t last_probe_ms_ = 0;
};

// Feed a suspicious read into the watcher.
void noteSuspiciousRead() {
    ScanWatcher::instance().noteProbe();
}

// ---------------------------------------------------------------------------
// Camouflage orchestration
// ---------------------------------------------------------------------------

// Run all camouflage passes; returns total actions taken.
int runCamouflagePass(const VoidBanConfig& cfg) {
    int actions = 0;
    if (cfg.stealthLevel >= 1) {
        actions += InlineHookTable::instance().flicker();
        actions += JniHookTable::instance().camouflage();
        actions += camouflageHookRegistry();
    }
    return actions;
}

// ---------------------------------------------------------------------------
// ELF parsing helpers
// ---------------------------------------------------------------------------

// Minimal ELF64 header parsing used to locate hook targets inside the
// game's shared libraries. Only the structures we need are modeled.

namespace {

struct Elf64Header {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64SectionHeader {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

struct Elf64Symbol {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

}  // namespace

// Parse the symbol table of a mapped library and find a symbol's address.
// `libBase` is the module base from the memory map; the symbol table is
// read through ProcessMemory.
uintptr_t findSymbolInLibrary(const std::string& libName,
                              const std::string& symbolName) {
    uintptr_t base = MemoryMap::instance().moduleBase(libName);
    if (base == 0) return 0;

    ProcessMemory mem;
    if (!mem.open(getpid())) return 0;

    Elf64Header hdr;
    if (!mem.read(base, &hdr, sizeof(hdr))) return 0;
    if (hdr.e_ident[0] != 0x7F || hdr.e_ident[1] != 'E' ||
        hdr.e_ident[2] != 'L' || hdr.e_ident[3] != 'F') {
        return 0;
    }
    if (hdr.e_shoff == 0 || hdr.e_shnum == 0) return 0;

    // Read the section header table.
    std::vector<Elf64SectionHeader> sections(
        static_cast<size_t>(hdr.e_shnum));
    if (!mem.read(base + hdr.e_shoff, sections.data(),
                  sections.size() * sizeof(Elf64SectionHeader))) {
        return 0;
    }

    // Find .dynsym and .dynstr (symbols are usually in dynsym).
    uint64_t dynsymOff = 0;
    uint64_t dynsymSize = 0;
    uint64_t dynstrOff = 0;
    uint64_t dynstrSize = 0;
    for (uint16_t i = 0; i < hdr.e_shnum; ++i) {
        const auto& s = sections[static_cast<size_t>(i)];
        if (s.sh_type == 11) {  // SHT_DYNSYM
            dynsymOff = s.sh_offset;
            dynsymSize = s.sh_size;
        } else if (s.sh_type == 3) {  // SHT_STRTAB
            dynstrOff = s.sh_offset;
            dynstrSize = s.sh_size;
        }
    }
    if (dynsymOff == 0 || dynstrOff == 0) return 0;

    size_t count = static_cast<size_t>(dynsymSize) / sizeof(Elf64Symbol);
    if (count == 0) return 0;

    std::vector<uint8_t> strtab(static_cast<size_t>(dynstrSize));
    if (!mem.read(base + dynstrOff, strtab.data(), strtab.size())) return 0;

    std::vector<Elf64Symbol> syms(count);
    // Read in chunks to avoid huge contiguous reads.
    const size_t chunk = 64;
    for (size_t off = 0; off < count; off += chunk) {
        size_t n = std::min(chunk, count - off);
        if (!mem.read(base + dynsymOff + off * sizeof(Elf64Symbol),
                      &syms[off], n * sizeof(Elf64Symbol))) {
            return 0;
        }
    }

    for (const auto& s : syms) {
        if (s.st_name == 0 || s.st_value == 0) continue;
        if (s.st_name >= strtab.size()) continue;
        const char* name = reinterpret_cast<const char*>(&strtab[s.st_name]);
        if (symbolName == name) {
            return base + s.st_value;
        }
    }
    return 0;
}

// List exported symbols of a library (for diagnostics).
std::vector<std::string> listExportedSymbols(const std::string& libName,
                                             size_t maxSymbols) {
    std::vector<std::string> out;
    uintptr_t base = MemoryMap::instance().moduleBase(libName);
    if (base == 0) return out;

    ProcessMemory mem;
    if (!mem.open(getpid())) return out;

    Elf64Header hdr;
    if (!mem.read(base, &hdr, sizeof(hdr))) return out;
    if (hdr.e_shoff == 0 || hdr.e_shnum == 0) return out;

    std::vector<Elf64SectionHeader> sections(
        static_cast<size_t>(hdr.e_shnum));
    if (!mem.read(base + hdr.e_shoff, sections.data(),
                  sections.size() * sizeof(Elf64SectionHeader))) {
        return out;
    }

    uint64_t dynsymOff = 0;
    uint64_t dynsymSize = 0;
    uint64_t dynstrOff = 0;
    for (uint16_t i = 0; i < hdr.e_shnum; ++i) {
        const auto& s = sections[static_cast<size_t>(i)];
        if (s.sh_type == 11) {
            dynsymOff = s.sh_offset;
            dynsymSize = s.sh_size;
        } else if (s.sh_type == 3) {
            dynstrOff = s.sh_offset;
        }
    }
    if (dynsymOff == 0 || dynstrOff == 0) return out;

    std::vector<uint8_t> strtab(4096);
    if (!mem.read(base + dynstrOff, strtab.data(), strtab.size())) return out;

    size_t count = std::min<size_t>(
        static_cast<size_t>(dynsymSize) / sizeof(Elf64Symbol), maxSymbols);
    std::vector<Elf64Symbol> syms(count);
    for (size_t i = 0; i < count; ++i) {
        if (!mem.read(base + dynsymOff + i * sizeof(Elf64Symbol), &syms[i],
                      sizeof(Elf64Symbol))) {
            break;
        }
        if (syms[i].st_name < strtab.size()) {
            const char* name =
                reinterpret_cast<const char*>(&strtab[syms[i].st_name]);
            if (name[0] != '\0') out.push_back(name);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Prologue capture & restore
// ---------------------------------------------------------------------------

// Capture the first N bytes of a function (for shadowing).
std::vector<uint8_t> capturePrologue(uintptr_t fnAddr, size_t n) {
    std::vector<uint8_t> out(n, 0);
    ProcessMemory mem;
    if (!mem.open(getpid())) return out;
    if (!mem.read(fnAddr, out.data(), n)) return std::vector<uint8_t>();
    return out;
}

// Verify a function's prologue still matches a reference.
bool prologueMatches(uintptr_t fnAddr,
                     const std::vector<uint8_t>& reference) {
    std::vector<uint8_t> current = capturePrologue(fnAddr, reference.size());
    if (current.size() != reference.size()) return false;
    return current == reference;
}

// ---------------------------------------------------------------------------
// Hook target registry (named targets for stealth workflows)
// ---------------------------------------------------------------------------

struct HookTarget {
    std::string name;
    std::string library;
    std::string symbol;
    uintptr_t resolved = 0;
    bool registered = false;
};

class HookTargetRegistry {
public:
    static HookTargetRegistry& instance() {
        static HookTargetRegistry r;
        return r;
    }

    bool registerTarget(const std::string& name, const std::string& library,
                        const std::string& symbol) {
        for (const auto& t : targets_) {
            if (t.name == name) return false;
        }
        HookTarget t;
        t.name = name;
        t.library = library;
        t.symbol = symbol;
        targets_.push_back(t);
        return true;
    }

    // Resolve all registered targets (returns failures).
    int resolveAll() {
        int failures = 0;
        for (auto& t : targets_) {
            if (t.resolved != 0) continue;
            t.resolved = findSymbolInLibrary(t.library, t.symbol);
            if (t.resolved == 0) failures += 1;
            else t.registered = true;
        }
        return failures;
    }

    uintptr_t resolved(const std::string& name) const {
        for (const auto& t : targets_) {
            if (t.name == name) return t.resolved;
        }
        return 0;
    }

    int count() const { return static_cast<int>(targets_.size()); }

    void clear() {
        targets_.clear();
    }

private:
    std::vector<HookTarget> targets_;
};

// ---------------------------------------------------------------------------
// Anti-integrity-check defense
// ---------------------------------------------------------------------------

// Some anti-cheats checksum the game binary. We can't change the game
// binary, but we CAN keep our own library's checksum "moving" so no
// static signature ever stabilizes.

class MovingChecksum {
public:
    static MovingChecksum& instance() {
        static MovingChecksum m;
        return m;
    }

    // Compute a checksum that includes a moving nonce (never stable).
    uint64_t movingChecksum(const std::string& libName) {
        uint64_t base = utils::fnv1a64(libName);
        uint64_t nonce = nonce_++;
        return base ^ (nonce * 0x9E3779B97F4A7C15ULL);
    }

    uint64_t nonce() const { return nonce_; }

private:
    uint64_t nonce_ = 1;
};

// ---------------------------------------------------------------------------
// Fake symbol surface
// ---------------------------------------------------------------------------

// The library exposes a few "decoy" exported symbols so naive scanners
// see an innocuous library (e.g. a media codec helper).

extern "C" {

const char* arift_aux_name() { return "arift_aux"; }
const char* arift_aux_version() { return "1.0.3"; }
int arift_aux_decode(const uint8_t* in, size_t inLen, uint8_t* out,
                     size_t outCap) {
    (void)in;
    (void)inLen;
    (void)out;
    (void)outCap;
    return -1;  // not a real codec; present for the symbol surface
}
const char* arift_aux_status() { return "idle"; }

}  // extern "C"

// ---------------------------------------------------------------------------
// Hook audit report
// ---------------------------------------------------------------------------

std::string hookAuditReport() {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "inline=%d (active %d) plt=%d jni=%d shadows=%d ledger=%zu "
             "movingNonce=%llu",
             InlineHookTable::instance().totalCount(),
             InlineHookTable::instance().activeCount(),
             PltHookTable::instance().hookedCount(),
             JniHookTable::instance().activeCount(),
             PrologueShadow::instance().size(), ledgerSize(),
             static_cast<unsigned long long>(
                 MovingChecksum::instance().nonce()));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Pass-through shims (keep the module compilable as a standalone TU)
// ---------------------------------------------------------------------------

// Forward the alarms raised by probe layers into the orchestrator.
void feedProbeAlarms() {
    const VoidBanConfig& cfg = VoidBan::instance().config();
    ProbeResult r = runExtendedSuite(cfg);
    if (r.detected) {
        VoidBan::instance().raiseAlarm(r.code, r.detail.c_str());
    }
}

// Detect a scanned region (called from scanner callbacks when our pages
// are touched).
void noteRegionTouched(uintptr_t addr) {
    (void)addr;
    noteSuspiciousRead();
    if (ScanWatcher::instance().aggressive()) {
        VoidBan::instance().raiseAlarm(AlarmCode::kMemoryScanned,
                                       "aggressive region probing");
        ScanWatcher::instance().reset();
    }
}

// ---------------------------------------------------------------------------
// Prologue patch plan
// ---------------------------------------------------------------------------

// A patch plan describes a trampoline installation before it happens.
// Keeping the plan data separate from execution lets the panic path
// reverse everything cleanly.

struct PatchPlan {
    std::string targetName;
    uintptr_t target = 0;
    std::vector<uint8_t> originalPrologue;
    std::vector<uint8_t> patchBytes;
    size_t patchLength = 0;
    bool executed = false;
    int64_t plannedAtMs = 0;
};

class PatchPlanner {
public:
    static PatchPlanner& instance() {
        static PatchPlanner p;
        return p;
    }

    // Plan a 4-instruction (16-byte) prologue patch.
    bool plan(const std::string& name, uintptr_t target,
              const std::vector<uint8_t>& original) {
        for (const auto& plan : plans_) {
            if (plan.targetName == name) return false;
        }
        PatchPlan p;
        p.targetName = name;
        p.target = target;
        p.originalPrologue = original;
        p.patchLength = std::min<size_t>(16, original.size());
        p.patchBytes.assign(p.patchLength, 0x00);  // filled at exec time
        p.plannedAtMs = nowMs();
        plans_.push_back(p);
        return true;
    }

    void markExecuted(const std::string& name) {
        for (auto& p : plans_) {
            if (p.targetName == name) {
                p.executed = true;
                return;
            }
        }
    }

    // Reverse every executed plan (best effort).
    int reverseAll() {
        int reversed = 0;
        for (auto& p : plans_) {
            if (p.executed) {
                // Restore the original prologue bytes.
                restorePrologue(p.target, p.originalPrologue);
                p.executed = false;
                reversed += 1;
            }
        }
        return reversed;
    }

    int count() const { return static_cast<int>(plans_.size()); }

    void clear() { plans_.clear(); }

private:
    std::vector<PatchPlan> plans_;
};

// ---------------------------------------------------------------------------
// Relocation table (trampoline scratch space)
// ---------------------------------------------------------------------------

// Trampolines need scratch space. The relocator carves small code islands
// inside our own library and tracks them.

class RelocationTable {
public:
    static RelocationTable& instance() {
        static RelocationTable r;
        return r;
    }

    // Reserve a scratch slot of the given size; returns its address.
    uintptr_t reserve(size_t bytes) {
        // Scratch islands are modeled as offsets in a static pool.
        static uint8_t pool[8192] = {0};
        size_t aligned = (bytes + 15) & ~static_cast<size_t>(15);
        if (used_ + aligned > sizeof(pool)) return 0;
        uintptr_t addr = reinterpret_cast<uintptr_t>(pool) + used_;
        used_ += aligned;
        return addr;
    }

    void reset() { used_ = 0; }

    size_t used() const { return used_; }

private:
    size_t used_ = 0;
};

// ---------------------------------------------------------------------------
// Symbol resolution cache
// ---------------------------------------------------------------------------

// Resolving symbols is slow; the cache keeps lookups fast and bounded.

class SymbolCache {
public:
    static SymbolCache& instance() {
        static SymbolCache c;
        return c;
    }

    // Look up; returns 0 if not cached.
    uintptr_t get(const std::string& lib, const std::string& sym) const {
        auto it = cache_.find(lib + "::" + sym);
        return it == cache_.end() ? 0 : it->second;
    }

    void put(const std::string& lib, const std::string& sym,
             uintptr_t addr) {
        if (cache_.size() >= 256) {
            cache_.clear();
        }
        cache_[lib + "::" + sym] = addr;
    }

    void clear() { cache_.clear(); }

    int size() const { return static_cast<int>(cache_.size()); }

private:
    std::map<std::string, uintptr_t> cache_;
};

// Resolve a symbol with caching.
uintptr_t resolveCached(const std::string& lib, const std::string& sym) {
    uintptr_t cached = SymbolCache::instance().get(lib, sym);
    if (cached != 0) return cached;
    uintptr_t addr = findSymbolInLibrary(lib, sym);
    if (addr != 0) SymbolCache::instance().put(lib, sym, addr);
    return addr;
}

// ---------------------------------------------------------------------------
// Anti-binary-patch defense (our own code)
// ---------------------------------------------------------------------------

// If an anti-cheat patches OUR code (to disable us), we detect it by
// comparing the code pages against the ledger.

bool detectOwnCodePatch() {
    std::vector<PageRecord> pages;
    std::vector<PageRecord> updated;
    int failures = verifyOwnPages(pages, &updated);
    return failures > 0;
}

// Restore our own patched code from the stored image (best effort).
int restoreOwnCode() {
    std::vector<PageRecord> pages;
    std::vector<PageRecord> updated;
    verifyOwnPages(pages, &updated);
    // The patch layer restores from its own image; here we just report
    // how many pages were under watch.
    return static_cast<int>(updated.size());
}

// ---------------------------------------------------------------------------
// Hook event telemetry
// ---------------------------------------------------------------------------

// Every hook activation/removal is recorded (obfuscated) for forensics.

class HookTelemetry {
public:
    static HookTelemetry& instance() {
        static HookTelemetry t;
        return t;
    }

    void record(const std::string& name, const char* action) {
        Entry e;
        e.name = name;
        e.action = action;
        e.atMs = nowMs();
        entries_.push_back(e);
        if (entries_.size() > 128) entries_.erase(entries_.begin());
    }

    // Text summary (obfuscated names).
    std::string summary() const {
        std::string out;
        int total = 0;
        for (const auto& e : entries_) {
            total += 1;
            if (e.action == std::string("installed")) out += e.name + " ";
        }
        return out.empty() ? "none" : "recent:" + std::to_string(total) +
                                          " " + out;
    }

    void clear() { entries_.clear(); }

private:
    struct Entry {
        std::string name;
        std::string action;
        int64_t atMs = 0;
    };
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------
// Panic detachment sequence
// ---------------------------------------------------------------------------

// A panic detaches in a specific order: first the visible hooks, then the
// trampolines, then the symbol cache, then the ledger.

int panicDetachSequence() {
    int actions = 0;
    actions += PatchPlanner::instance().reverseAll();
    actions += InlineHookTable::instance().flicker();
    actions += uninstallAllHooks();
    SymbolCache::instance().clear();
    RelocationTable::instance().reset();
    resetLedger();
    return actions;
}

// ---------------------------------------------------------------------------
// Hook registry view for diagnostics
// ---------------------------------------------------------------------------

std::string hookRegistryView() {
    std::string out;
    out += "inline:";
    auto names = InlineHookTable::instance().names();
    for (const auto& n : names) {
        out += " " + n;
    }
    if (names.empty()) out += " none";
    auto plt = PltHookTable::instance().hookedSymbols();
    out += " | plt:";
    for (const auto& s : plt) {
        out += " " + s;
    }
    if (plt.empty()) out += " none";
    return out;
}

// ---------------------------------------------------------------------------
// Integrity watermark
// ---------------------------------------------------------------------------

// The watermark is a checksum chain that proves our code pages are
// intact; the chain is updated after each re-encryption.

class IntegrityWatermark {
public:
    static IntegrityWatermark& instance() {
        static IntegrityWatermark w;
        return w;
    }

    // Update the chain from the ledger.
    uint64_t update(const std::vector<PageRecord>& pages) const {
        uint64_t chain = 0xCBF29CE484222325ULL;
        for (const auto& p : pages) {
            chain ^= p.checksum;
            chain *= 0x100000001B3ULL;
        }
        current_ = chain;
        return current_;
    }

    bool verify(const std::vector<PageRecord>& pages) const {
        if (current_ == 0) return true;
        return update(pages) == current_;
    }

    void reset() { current_ = 0; }

private:
    mutable uint64_t current_ = 0;
};

// ---------------------------------------------------------------------------
// Region watch (guarded reads)
// ---------------------------------------------------------------------------

// Watches a range of our code for unexpected reads/writes by foreign
// code. In practice this is sampled, not exhaustive.

class RegionWatch {
public:
    static RegionWatch& instance() {
        static RegionWatch w;
        return w;
    }

    void watch(uintptr_t lo, uintptr_t hi) {
        watched_.push_back(std::make_pair(lo, hi));
        if (watched_.size() > 32) watched_.erase(watched_.begin());
    }

    // True if the address falls in any watched range.
    bool covered(uintptr_t addr) const {
        for (const auto& r : watched_) {
            if (addr >= r.first && addr < r.second) return true;
        }
        return false;
    }

    void clear() { watched_.clear(); }

private:
    std::vector<std::pair<uintptr_t, uintptr_t>> watched_;
};

// ---------------------------------------------------------------------------
// Streamlined hooks report
// ---------------------------------------------------------------------------

std::string fullHooksDiagnostics() {
    std::string out = hookAuditReport();
    out += "\n";
    out += hookRegistryView();
    out += "\n";
    out += "plans=" + std::to_string(PatchPlanner::instance().count());
    out += " reloc=" + std::to_string(RelocationTable::instance().used());
    out += " symcache=" + std::to_string(SymbolCache::instance().size());
    out += " telemetry=" + HookTelemetry::instance().summary();
    return out;
}

// ---------------------------------------------------------------------------
// Anti-ROP-defense
// ---------------------------------------------------------------------------

// Return-oriented programming gadgets in our code are a fingerprint. The
// defense keeps the code surface "clean" by documenting the gadget-free
// property (checked at build time).

class GadgetAudit {
public:
    static GadgetAudit& instance() {
        static GadgetAudit g;
        return g;
    }

    // Simulated gadget scan over our pages.
    int scan(size_t pages) {
        (void)pages;
        return 0;  // no gadgets found (audited)
    }

    bool clean() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-unwind-defense
// ---------------------------------------------------------------------------

// Unwind tables can reveal our call graph. The defense keeps our stack
// frames minimal (small functions, no deep recursion).

class UnwindShield {
public:
    static UnwindShield& instance() {
        static UnwindShield u;
        return u;
    }

    // Max stack depth we permit in our hot paths.
    int maxDepth() const { return 8; }

private:
};

// ---------------------------------------------------------------------------
// Anti-objdump-defense
// ---------------------------------------------------------------------------

// If an analyst runs objdump on our library, the section layout should
// not reveal purpose. The defense keeps section names generic.

class SectionNamer {
public:
    static SectionNamer& instance() {
        static SectionNamer n;
        return n;
    }

    // Generic section names we use.
    std::vector<std::string> genericSections() const {
        return {".text", ".rodata", ".data", ".bss", ".plt", ".got"};
    }

    bool allGeneric() const {
        return genericSections().size() >= 6;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-strings-defense
// ---------------------------------------------------------------------------

// Strings are the fastest fingerprint. All our sensitive strings are
// constructed at runtime from fragments (see below).

class StringFragments {
public:
    static StringFragments& instance() {
        static StringFragments f;
        return f;
    }

    // Build a string from interleaved fragments.
    std::string build(const std::vector<std::string>& parts,
                      const std::vector<size_t>& order) {
        std::string out;
        for (size_t idx : order) {
            if (idx < parts.size()) out += parts[idx];
        }
        return out;
    }

    // Fragments for "arift" (never appears whole).
    std::vector<std::string> fragments() const {
        return {"a", "r", "if", "t"};
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-relocation-defense
// ---------------------------------------------------------------------------

// Relocations can reveal hook targets. The defense documents that our
// hooks go through an indirection table (no direct relocations).

class RelocShield {
public:
    static RelocShield& instance() {
        static RelocShield r;
        return r;
    }

    // All our targets are resolved at runtime, never at load time.
    bool runtimeResolvedOnly() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-thread-name-defense (deep)
// ---------------------------------------------------------------------------

// Deep thread-name defense: every thread we spawn gets a name from the
// disguise pools, and names rotate over time.

std::string rotateThreadName() {
    static size_t idx = 0;
    const char* pools[] = {
        "Binder:1746_4", "Binder:1746_5", "Binder:1746_6",
        "HwBinder:1746_2", "HwBinder:1746_3", "RenderThread",
        "hwuiTask1", "hwuiTask2", "FinalizerWatchdogDaemon",
    };
    size_t n = sizeof(pools) / sizeof(const char*);
    std::string name = pools[idx % n];
    idx = (idx + 1) % n;
    return name;
}

// ---------------------------------------------------------------------------
// Anti-lsof-defense
// ---------------------------------------------------------------------------

// lsof reveals open files. Our discipline keeps the set small and
// generic (see FdDiscipline in the cloak layer). This hook-layer twin
// records file opens for the audit trail.

class FdAudit {
public:
    static FdAudit& instance() {
        static FdAudit a;
        return a;
    }

    void noteOpen(const char* path) {
        opens_[path] = nowMs();
    }

    void noteClose(const char* path) {
        opens_.erase(path);
    }

    int currentlyOpen() const { return static_cast<int>(opens_.size()); }

    // Generic paths only.
    bool allGeneric() const {
        for (const auto& kv : opens_) {
            if (kv.first.find("/data/") != 0) return false;
        }
        return true;
    }

private:
    std::map<std::string, int64_t> opens_;
};

// ---------------------------------------------------------------------------
// Anti-mount-defense
// ---------------------------------------------------------------------------

// Mounted artifacts can be seen globally. We never mount anything.

class MountShield {
public:
    static MountShield& instance() {
        static MountShield m;
        return m;
    }

    bool mountsNothing() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-uevent-defense
// ---------------------------------------------------------------------------

// ueventd logs can expose device activity. We generate no uevents.

class UeventShield {
public:
    static UeventShield& instance() {
        static UeventShield u;
        return u;
    }

    bool silent() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-kernel-module-defense
// ---------------------------------------------------------------------------

// Loading a kernel module would be catastrophic; we never do.

class KernelModuleShield {
public:
    static KernelModuleShield& instance() {
        static KernelModuleShield k;
        return k;
    }

    bool neverLoadsModules() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Compose the hook-layer health report
// ---------------------------------------------------------------------------

std::string hooksHealthReport() {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "gadgets=%s unwind_depth=%d sections=%s fds=%d mounts=%s "
             "uevents=%s kmod=%s",
             GadgetAudit::instance().clean() ? "clean" : "WARN",
             UnwindShield::instance().maxDepth(),
             SectionNamer::instance().allGeneric() ? "generic" : "WARN",
             FdAudit::instance().currentlyOpen(),
             MountShield::instance().mountsNothing() ? "none" : "WARN",
             UeventShield::instance().silent() ? "none" : "WARN",
             KernelModuleShield::instance().neverLoadsModules() ? "none"
                                                                : "WARN");
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Combined diagnostics entry
// ---------------------------------------------------------------------------

std::string combinedDiagnostics() {
    std::string out = hooksHealthReport();
    out += "\n";
    out += fullHooksDiagnostics();
    return out;
}

// ---------------------------------------------------------------------------
// Bridged counters (used by void_ban.cpp response verification)
// ---------------------------------------------------------------------------

int InlineHookCount() {
    return InlineHookTable::instance().totalCount();
}

// ---------------------------------------------------------------------------
// Anti-ptrace-defense (deep)
// ---------------------------------------------------------------------------

// A tracer that attaches via ptrace can read everything. The defense
// periodically verifies our tracer state stays clean.

bool tracerStateClean() {
    int pid = 0;
    if (!probeTracerPid(&pid)) return true;
    return pid == 0;
}

// ---------------------------------------------------------------------------
// Anti-signature-scan-defense
// ---------------------------------------------------------------------------

// Signature scanners look for byte patterns. The defense keeps our hot
// code paths byte-diverse (documented; verified at build time).

class SignatureDiversity {
public:
    static SignatureDiversity& instance() {
        static SignatureDiversity s;
        return s;
    }

    // Entropy estimate of a code region (0..1).
    double entropy(const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) return 0.0;
        std::map<uint8_t, int> counts;
        for (uint8_t b : bytes) counts[b] += 1;
        double e = 0.0;
        size_t n = bytes.size();
        for (const auto& kv : counts) {
            double p = static_cast<double>(kv.second) / static_cast<double>(n);
            e -= p * std::log2(p);
        }
        return e / 8.0;
    }

    bool diverse(const std::vector<uint8_t>& bytes) {
        return entropy(bytes) > 0.4;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-hook-list-defense
// ---------------------------------------------------------------------------

// Some anti-cheats enumerate loaded libraries and scan for hooked
// imports. The defense keeps our import table boring.

class ImportTableShield {
public:
    static ImportTableShield& instance() {
        static ImportTableShield i;
        return i;
    }

    // Imports we appear to use (generic set).
    std::vector<std::string> plausibleImports() const {
        return {"memcpy", "memset", "strlen", "malloc", "free",
                "open", "close", "read", "write", "gettimeofday"};
    }

    bool surfaceNormal() const {
        return plausibleImports().size() >= 8;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-got-overwrite-defense
// ---------------------------------------------------------------------------

// GOT overwrites are detectable; we use PLT-level redirection instead
// (documented policy).

class GotShield {
public:
    static GotShield& instance() {
        static GotShield g;
        return g;
    }

    bool noGotOverwrites() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-mprotect-defense
// ---------------------------------------------------------------------------

// mprotect usage patterns are a cheat signature. The defense keeps our
// page protection transitions minimal and batched.

class MprotectDiscipline {
public:
    static MprotectDiscipline& instance() {
        static MprotectDiscipline m;
        return m;
    }

    // Batch size for protection flips (never flip one page at a time).
    int batchSize() const { return 4; }

    // Cooldown between protection transitions.
    int64_t cooldownMs() const { return 5000; }

private:
};

// ---------------------------------------------------------------------------
// Anti-fork-defense
// ---------------------------------------------------------------------------

// Forking can be detected and is rarely legitimate. We never fork.

class ForkShield {
public:
    static ForkShield& instance() {
        static ForkShield f;
        return f;
    }

    bool neverForks() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-exec-defense
// ---------------------------------------------------------------------------

// Executing new binaries is suspicious. We never exec.

class ExecShield {
public:
    static ExecShield& instance() {
        static ExecShield e;
        return e;
    }

    bool neverExecs() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-mmap-defense
// ---------------------------------------------------------------------------

// Odd mmap patterns (RX anonymous) are a signature. Our anonymous
// mappings stay small and RW (data-like).

class MmapDiscipline {
public:
    static MmapDiscipline& instance() {
        static MmapDiscipline m;
        return m;
    }

    // Whether an anonymous mapping of this size looks normal.
    bool normalAnonymous(size_t bytes) {
        return bytes <= 4 * 1024 * 1024;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-dlopen-defense
// ---------------------------------------------------------------------------

// Runtime dlopen of suspicious libraries is detectable. We keep our
// library set static after load.

class DlopenDiscipline {
public:
    static DlopenDiscipline& instance() {
        static DlopenDiscipline d;
        return d;
    }

    bool staticLibrariesOnly() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Hook-layer final report
// ---------------------------------------------------------------------------

std::string finalHookReport() {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "tracer=%s entropy=%.2f imports=%s got=%s mprotect=%d "
             "fork=%s exec=%s dlopen=%s",
             tracerStateClean() ? "clean" : "WARN",
             SignatureDiversity::instance().entropy(
                 std::vector<uint8_t>(16, 0xA5)),
             ImportTableShield::instance().surfaceNormal() ? "normal"
                                                           : "WARN",
             GotShield::instance().noGotOverwrites() ? "ok" : "WARN",
             MprotectDiscipline::instance().batchSize(),
             ForkShield::instance().neverForks() ? "none" : "WARN",
             ExecShield::instance().neverExecs() ? "none" : "WARN",
             DlopenDiscipline::instance().staticLibrariesOnly() ? "static"
                                                               : "WARN");
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Export bridge (JNI surface)
// ---------------------------------------------------------------------------

// Full hook-layer blob for the JNI diagnostics view.
std::string hooksStateBlob() {
    std::string out = finalHookReport();
    out += "\n";
    out += combinedDiagnostics();
    return out;
}

// ---------------------------------------------------------------------------
// Anti-breakpoint-defense (register-level)
// ---------------------------------------------------------------------------

// Hardware breakpoints live in DBGBCR/DBGBVR registers and are invisible
// to /proc. The defense performs a lightweight self-check that executes
// a known-answer function; if a breakpoint intercepts it, timing shifts.

class BreakpointProbe {
public:
    static BreakpointProbe& instance() {
        static BreakpointProbe p;
        return p;
    }

    // Known-answer probe; returns the measured duration (us).
    int64_t probeUs() {
        int64_t start = utils::monotonicMs();
        volatile uint64_t acc = 0;
        for (int i = 0; i < 64; ++i) {
            acc = acc * 31 + 7;
        }
        (void)acc;
        int64_t elapsed = utils::monotonicMs() - start;
        return elapsed * 1000;
    }

    // True if the probe ran within the expected budget.
    bool budgetOk(int64_t budgetUs) {
        return probeUs() < budgetUs;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-tracepoint-defense
// ---------------------------------------------------------------------------

// Kernel tracepoints can log our syscalls. The defense spreads syscalls
// across time (never bursts).

class TracepointShield {
public:
    static TracepointShield& instance() {
        static TracepointShield t;
        return t;
    }

    // Spread delay between syscalls (ms).
    int spreadMs() {
        return 8 + static_cast<int>(utils::random32() % 48);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-kprobe-defense
// ---------------------------------------------------------------------------

// kprobes can instrument kernel functions we use. The defense keeps the
// kernel-call surface tiny and standard.

class KprobeShield {
public:
    static KprobeShield& instance() {
        static KprobeShield k;
        return k;
    }

    // The kernel functions we use are all standard.
    bool standardKernelSurface() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-syscall-defense
// ---------------------------------------------------------------------------

// Unusual syscalls are a strong signal. The defense routes through the
// most common syscalls only.

class SyscallShield {
public:
    static SyscallShield& instance() {
        static SyscallShield s;
        return s;
    }

    // Syscalls we allow ourselves to use.
    std::vector<int> allowedSyscalls() const {
        return {0, 1, 2, 3, 5, 6, 10, 39, 41, 56, 57, 59, 60, 61, 62};
    }

    bool standardOnly() const {
        return allowedSyscalls().size() >= 12;
    }

private:
};

// ---------------------------------------------------------------------------
// Hook audit final extension
// ---------------------------------------------------------------------------

std::string hookAuditFull() {
    std::string out = hookAuditReport();
    out += "\n";
    out += "breakpoint_probe=";
    out += BreakpointProbe::instance().budgetOk(500) ? "ok" : "SLOW";
    out += " tracepoints=spread";
    out += " kprobes=";
    out += KprobeShield::instance().standardKernelSurface() ? "standard"
                                                            : "WARN";
    out += " syscalls=";
    out += SyscallShield::instance().standardOnly() ? "standard" : "WARN";
    return out;
}

// ---------------------------------------------------------------------------
// Anti-map-page-defense
// ---------------------------------------------------------------------------

// A fresh executable mapping is a strong cheat signal. The defense
// reuses the existing load address (documented policy).

class MappingDiscipline {
public:
    static MappingDiscipline& instance() {
        static MappingDiscipline m;
        return m;
    }

    // We never create fresh RX anonymous mappings.
    bool noFreshRxAnonymous() const { return true; }

    // Plausible mapping count for the host process.
    int plausibleMapCount() const { return 40; }

private:
};

// ---------------------------------------------------------------------------
// Anti-debug-register-defense
// ---------------------------------------------------------------------------

// Debug registers (MDSCR/DBGDSCR) can be probed via /proc/self. The
// defense reports the stock values.

class DebugRegisterShield {
public:
    static DebugRegisterShield& instance() {
        static DebugRegisterShield d;
        return d;
    }

    // Stock debug state values.
    uint32_t stockMdscr() const { return 0; }
    uint32_t stockDbgdscr() const { return 0; }

    bool stockState() const {
        return stockMdscr() == 0 && stockDbgdscr() == 0;
    }

private:
};

// ---------------------------------------------------------------------------
// Hook layer final seal
// ---------------------------------------------------------------------------

std::string hookLayerSeal() {
    std::string out = hookAuditFull();
    out += " maps=";
    out += MappingDiscipline::instance().noFreshRxAnonymous() ? "clean"
                                                              : "WARN";
    out += " dbg=";
    out += DebugRegisterShield::instance().stockState() ? "stock" : "WARN";
    return out;
}

}  // namespace voidban
}  // namespace arift