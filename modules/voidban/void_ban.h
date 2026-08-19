#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "arift_utils.h"

namespace arift {
namespace voidban {

// Feature identifier used by FeatureSwitch (see feature_switch.h).
constexpr int kFeatureVoidBan = 9;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct VoidBanConfig {
    bool enabled = false;

    // Stealth posture: 0 = light, 1 = balanced, 2 = paranoid.
    int stealthLevel = 1;

    // Heartbeat: periodic fake activity to look like a normal library.
    int heartbeatIntervalMs = 45000;
    int decoyIntervalMs = 90000;

    // Integrity: re-checksum our own code pages.
    int integrityCheckMs = 60000;
    int memoryAuditMs = 300000;

    // Anti-debugger checks.
    bool checkTracerPid = true;
    bool checkPtrace = true;
    bool checkFrida = true;
    bool checkXposed = true;
    bool checkMagisk = true;

    // Cloaking.
    bool cloakThreads = true;
    bool cloakProcessName = true;
    bool cloakMaps = true;
    bool cloakNetwork = true;
    bool obfuscateResources = true;

    // Panic behavior.
    int panicThreshold = 3;        // alarms before hard panic
    bool panicDisablesModules = true;
    bool panicDetachesHooks = true;
    bool panicHidesProcess = true;

    // Self-modifying code cadence.
    int codeReencryptMs = 120000;

    // Decoy parameters.
    int decoyThreads = 2;
    int decoyAllocKb = 128;

    // Extra strings never logged (kept out of the binary image).
    std::vector<std::string> guardNames;
};

// ---------------------------------------------------------------------------
// Status / stats
// ---------------------------------------------------------------------------

struct VoidBanStats {
    bool active = false;
    int stealthLevel = 0;
    int64_t startedAtMs = 0;
    int64_t heartbeatsSent = 0;
    int64_t decoysSpawned = 0;
    int64_t integrityChecks = 0;
    int64_t integrityFailures = 0;
    int64_t memoryAudits = 0;
    int64_t alarmsRaised = 0;
    int64_t panicsTriggered = 0;
    int64_t hooksCamouflaged = 0;
    int64_t threadsCloaked = 0;
    int64_t mapsObfuscated = 0;
    int64_t codeReencrypts = 0;
    int64_t networkShaped = 0;
    int currentRisk = 0;       // 0..100
    std::string lastAlarm;
    std::string state = "idle";
};

// ---------------------------------------------------------------------------
// Alarm / event types
// ---------------------------------------------------------------------------

enum class AlarmCode : int {
    kNone = 0,
    kTracerDetected = 1,
    kPtraceDetected = 2,
    kFridaDetected = 3,
    kXposedDetected = 4,
    kMagiskDetected = 5,
    kIntegrityMismatch = 6,
    kHookDiscovered = 7,
    kMapsTampered = 8,
    kNetworkAnomaly = 9,
    kDebuggerBreakpoint = 10,
    kMemoryScanned = 11,
    kUnknownLibrary = 12,
    kFileSystemProbe = 13,
    kSelfModifyConflict = 14,
};

const char* alarmName(AlarmCode code);

// ---------------------------------------------------------------------------
// Integrity ledger
// ---------------------------------------------------------------------------

// Tracks checksums of our own code pages so tampering is detectable.
struct PageRecord {
    uintptr_t addr = 0;
    size_t size = 0;
    uint64_t checksum = 0;
    uint64_t lastVerifiedMs = 0;
    int failures = 0;
};

// ---------------------------------------------------------------------------
// Core orchestrator
// ---------------------------------------------------------------------------

class VoidBan {
public:
    static VoidBan& instance();

    void enable(const VoidBanConfig& cfg);
    void disable();
    bool enabled() const { return enabled_; }

    // Main pump (called by the host loop).
    void tick();

    // Match lifecycle hooks.
    void onMatchStart();
    void onMatchEnd();

    // Alarm entry point used by detection subsystems.
    void raiseAlarm(AlarmCode code, const char* detail);

    // Panic: emergency stealth mode.
    void panic();

    // UI-facing.
    VoidBanStats stats() const;
    std::string statusLine() const;
    std::string statusBlob() const;
    std::string riskLabel() const;

    // Manual commands (JNI).
    void runIntegrityCheckNow();
    void runMemoryAuditNow();
    void forceReencryptNow();
    void clearAlarms();

    // Risk model.
    int currentRisk() const { return risk_; }
    int alarmCount() const { return alarm_count_; }
    void applyRisk(int risk) { risk_ = utils::clamp(risk, 0, 100); }

    // Accessors.
    VoidBanConfig& config() { return cfg_; }
    const VoidBanConfig& config() const { return cfg_; }
    int64_t lastMatchStartMs() const { return match_start_ms_; }

    // Schedules the next event for each subsystem.
    int64_t nextHeartbeatMs() const { return next_heartbeat_ms_; }
    int64_t nextDecoyMs() const { return next_decoy_ms_; }
    int64_t nextIntegrityMs() const { return next_integrity_ms_; }
    int64_t nextAuditMs() const { return next_audit_ms_; }
    int64_t nextReencryptMs() const { return next_reencrypt_ms_; }

private:
    VoidBan() = default;
    bool enabled_ = false;
    VoidBanConfig cfg_;
    VoidBanStats stats_;
    int risk_ = 0;
    int alarm_count_ = 0;
    int64_t next_heartbeat_ms_ = 0;
    int64_t next_decoy_ms_ = 0;
    int64_t next_integrity_ms_ = 0;
    int64_t next_audit_ms_ = 0;
    int64_t next_reencrypt_ms_ = 0;
    int64_t last_tick_ms_ = 0;
    int64_t match_start_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Detection subsystem (vb_hooks.cpp)
// ---------------------------------------------------------------------------

// Probe results for one detection category.
struct ProbeResult {
    AlarmCode code = AlarmCode::kNone;
    bool detected = false;
    std::string detail;
    int severity = 0;  // 0..10
};

// Runs all enabled probes; returns the highest-severity finding (or none).
ProbeResult runDetectionSuite(const VoidBanConfig& cfg);

// Individual probes (each returns found/not found).
bool probeTracerPid(int* pid);
bool probePtraceAttach();
bool probeFridaPresence();
bool probeXposedPresence();
bool probeMagiskPresence();
bool probeDebuggerBreakpoints(uintptr_t rangeLo, uintptr_t rangeHi);

// Integrity verification of our own mapped code.
int verifyOwnPages(const std::vector<PageRecord>& pages,
                   std::vector<PageRecord>* updated);

// Camouflage: re-randomize hook order / names in the registry view.
int camouflageHookRegistry();

// Register an integrity watchdog callback.
void setIntegrityCallback(std::function<void(const std::vector<uint8_t>&)> cb);

// ---------------------------------------------------------------------------
// Cloaking subsystem (vb_cloak.cpp)
// ---------------------------------------------------------------------------

// Thread/process concealment.
int cloakThreadsNow(int64_t nowMs);
int cloakProcessNameNow();
int obfuscateMapsNow();
int shapeNetworkNow(int64_t nowMs);
int reencryptCodeNow(int64_t nowMs);

// Decoy manager.
int spawnDecoys(int count, int allocKb);
int pruneDecoys();
int decoyCount();

// Heartbeat.
void sendHeartbeat(int64_t nowMs);

// Resource obfuscation.
std::string encryptedLookup(const std::string& key);
void loadObfuscatedResources();

// Dynamic-analysis evasion helpers.
bool probeFridaRuntime();
bool probeXposedRuntime();
bool probeMapsCloakIntegrity();

// ---------------------------------------------------------------------------
// Host wiring helpers
// ---------------------------------------------------------------------------

// One-shot setup called at process start.
void installVoidBan(const VoidBanConfig& cfg);
void removeVoidBan();

// Toggle helper for JNI.
bool toggleEnabled();

// Full diagnostic blob.
std::string fullDiagnostics();

// Cloak upkeep (defined in vb_cloak.cpp, driven by the host loop).
void cloakUpkeep(int64_t nowMs);

}  // namespace voidban
}  // namespace arift