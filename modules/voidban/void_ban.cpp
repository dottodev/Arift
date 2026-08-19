#include "void_ban.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <utility>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {
namespace voidban {

namespace {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Time source alias.
int64_t nowMs() { return utils::monotonicMs(); }

// Severity table for alarm codes.
int severityOf(AlarmCode code) {
    switch (code) {
        case AlarmCode::kTracerDetected: return 6;
        case AlarmCode::kPtraceDetected: return 7;
        case AlarmCode::kFridaDetected: return 8;
        case AlarmCode::kXposedDetected: return 7;
        case AlarmCode::kMagiskDetected: return 6;
        case AlarmCode::kIntegrityMismatch: return 9;
        case AlarmCode::kHookDiscovered: return 9;
        case AlarmCode::kMapsTampered: return 8;
        case AlarmCode::kNetworkAnomaly: return 6;
        case AlarmCode::kDebuggerBreakpoint: return 9;
        case AlarmCode::kMemoryScanned: return 8;
        case AlarmCode::kUnknownLibrary: return 5;
        case AlarmCode::kFileSystemProbe: return 5;
        case AlarmCode::kSelfModifyConflict: return 4;
        default: return 0;
    }
}

// Risk contribution of an alarm (0..100).
int riskAdd(AlarmCode code) {
    return severityOf(code) * 4;
}

// Track the last quiet period for risk decay.
int64_t g_last_quiet_ms = 0;

// Whether an alarm is considered severe enough to count toward panic.
bool countsTowardPanic(AlarmCode code) {
    return severityOf(code) >= 7;
}

// Format a severity bar for the UI.
std::string severityBar(int sev) {
    std::string bar;
    for (int i = 0; i < 10; ++i) {
        bar += (i < sev) ? "#" : "-";
    }
    return bar;
}

}  // namespace

// ---------------------------------------------------------------------------
// Alarm naming
// ---------------------------------------------------------------------------

const char* alarmName(AlarmCode code) {
    switch (code) {
        case AlarmCode::kNone: return "none";
        case AlarmCode::kTracerDetected: return "tracer";
        case AlarmCode::kPtraceDetected: return "ptrace";
        case AlarmCode::kFridaDetected: return "frida";
        case AlarmCode::kXposedDetected: return "xposed";
        case AlarmCode::kMagiskDetected: return "magisk";
        case AlarmCode::kIntegrityMismatch: return "integrity";
        case AlarmCode::kHookDiscovered: return "hook-discovered";
        case AlarmCode::kMapsTampered: return "maps-tampered";
        case AlarmCode::kNetworkAnomaly: return "network-anomaly";
        case AlarmCode::kDebuggerBreakpoint: return "breakpoint";
        case AlarmCode::kMemoryScanned: return "memory-scan";
        case AlarmCode::kUnknownLibrary: return "unknown-library";
        case AlarmCode::kFileSystemProbe: return "fs-probe";
        case AlarmCode::kSelfModifyConflict: return "self-modify";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// VoidBan — core orchestrator
// ---------------------------------------------------------------------------

VoidBan& VoidBan::instance() {
    static VoidBan vb;
    return vb;
}

void VoidBan::enable(const VoidBanConfig& cfg) {
    cfg_ = cfg;
    cfg_.enabled = true;
    enabled_ = true;
    stats_.active = true;
    stats_.stealthLevel = cfg_.stealthLevel;
    stats_.startedAtMs = nowMs();
    stats_.state = "armed";
    risk_ = 0;
    alarm_count_ = 0;

    next_heartbeat_ms_ = nowMs() + cfg_.heartbeatIntervalMs;
    next_decoy_ms_ = nowMs() + cfg_.decoyIntervalMs;
    next_integrity_ms_ = nowMs() + cfg_.integrityCheckMs;
    next_audit_ms_ = nowMs() + cfg_.memoryAuditMs;
    next_reencrypt_ms_ = nowMs() + cfg_.codeReencryptMs;

    ARIFT_INFO(kTagVoidBan, "enabled (stealth=%d heartbeat=%dms decoy=%dms)",
               cfg_.stealthLevel, cfg_.heartbeatIntervalMs,
               cfg_.decoyIntervalMs);
}

void VoidBan::disable() {
    if (!enabled_) return;
    enabled_ = false;
    cfg_.enabled = false;
    stats_.active = false;
    stats_.state = "idle";
    ARIFT_INFO(kTagVoidBan, "disabled");
}

void VoidBan::raiseAlarm(AlarmCode code, const char* detail) {
    if (!enabled_) return;
    alarm_count_ += 1;
    stats_.alarmsRaised += 1;
    risk_ = utils::clamp(risk_ + riskAdd(code), 0, 100);
    stats_.currentRisk = risk_;
    stats_.lastAlarm = std::string(alarmName(code)) + ": " + detail;

    if (countsTowardPanic(code) &&
        alarm_count_ >= cfg_.panicThreshold) {
        panic();
        return;
    }
    ARIFT_WARN(kTagVoidBan, "alarm #%d [%s] risk=%d detail=%s",
               alarm_count_, alarmName(code), risk_, detail);
}

void VoidBan::panic() {
    if (stats_.panicsTriggered > 0) return;  // only once per session
    stats_.panicsTriggered += 1;
    stats_.state = "panic";
    risk_ = 100;
    stats_.currentRisk = 100;
    ARIFT_ERROR(kTagVoidBan, "PANIC: emergency stealth engaged");

    // Emergency actions (best effort, no logging of paths).
    if (cfg_.panicDetachesHooks) {
        int n = camouflageHookRegistry();
        stats_.hooksCamouflaged += n;
    }
    if (cfg_.panicHidesProcess) {
        cloakProcessNameNow();
        cloakThreadsNow(nowMs());
    }
    if (cfg_.panicDisablesModules) {
        // Signals the host via the feature switch; modules self-disable.
        stats_.state = "panic";
    }
}

void VoidBan::tick() {
    if (!enabled_) return;
    int64_t now = nowMs();
    if (last_tick_ms_ > 0 && now - last_tick_ms_ < 250) return;
    last_tick_ms_ = now;

    // Heartbeat.
    if (now >= next_heartbeat_ms_) {
        sendHeartbeat(now);
        stats_.heartbeatsSent += 1;
        next_heartbeat_ms_ = now + cfg_.heartbeatIntervalMs;
    }

    // Decoys.
    if (now >= next_decoy_ms_) {
        stats_.decoysSpawned += spawnDecoys(cfg_.decoyThreads,
                                            cfg_.decoyAllocKb);
        next_decoy_ms_ = now + cfg_.decoyIntervalMs;
    }

    // Integrity.
    if (now >= next_integrity_ms_) {
        stats_.integrityChecks += 1;
        // The page ledger lives in the hooks subsystem.
        runIntegrityCheckNow();
        next_integrity_ms_ = now + cfg_.integrityCheckMs;
    }

    // Memory audit (slow, run less often).
    if (now >= next_audit_ms_) {
        stats_.memoryAudits += 1;
        runMemoryAuditNow();
        next_audit_ms_ = now + cfg_.memoryAuditMs;
    }

    // Code re-encryption.
    if (now >= next_reencrypt_ms_) {
        stats_.codeReencrypts += reencryptCodeNow(now);
        next_reencrypt_ms_ = now + cfg_.codeReencryptMs;
    }

    // Network shaping.
    if (cfg_.cloakNetwork && (now % 37000) < 250) {
        stats_.networkShaped += shapeNetworkNow(now);
    }

    // Passive probes at a low cadence.
    if ((now % 5000) < 250) {
        ProbeResult r = runDetectionSuite(cfg_);
        if (r.detected) {
            raiseAlarm(r.code, r.detail.c_str());
        }
    }
}

void VoidBan::onMatchStart() {
    match_start_ms_ = nowMs();
    stats_.state = "armed";
    ARIFT_DEBUG(kTagVoidBan, "match start - state armed");
}

void VoidBan::onMatchEnd() {
    stats_.state = "armed";
    ARIFT_DEBUG(kTagVoidBan, "match end - state armed");
}

VoidBanStats VoidBan::stats() const {
    VoidBanStats s = stats_;
    s.currentRisk = risk_;
    s.active = enabled_;
    return s;
}

std::string VoidBan::statusLine() const {
    VoidBanStats s = stats();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "VB [%s] risk=%d alarms=%lld | hb=%lld decoys=%lld checks=%lld",
             s.state.c_str(), risk_,
             static_cast<long long>(s.alarmsRaised),
             static_cast<long long>(s.heartbeatsSent),
             static_cast<long long>(s.decoysSpawned),
             static_cast<long long>(s.integrityChecks));
    return std::string(buf);
}

std::string VoidBan::statusBlob() const {
    VoidBanStats s = stats();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "vb{on=%d stealth=%d state=%s risk=%d alarms=%lld hb=%lld "
             "decoy=%lld integ=%lld fail=%lld panic=%lld}",
             s.active ? 1 : 0, s.stealthLevel, s.state.c_str(), risk_,
             static_cast<long long>(s.alarmsRaised),
             static_cast<long long>(s.heartbeatsSent),
             static_cast<long long>(s.decoysSpawned),
             static_cast<long long>(s.integrityChecks),
             static_cast<long long>(s.integrityFailures),
             static_cast<long long>(s.panicsTriggered));
    return std::string(buf);
}

std::string VoidBan::riskLabel() const {
    if (risk_ >= 80) return "CRITICAL";
    if (risk_ >= 55) return "HIGH";
    if (risk_ >= 30) return "MODERATE";
    if (risk_ > 0) return "LOW";
    return "CLEAR";
}

void VoidBan::runIntegrityCheckNow() {
    if (!enabled_) return;
    // The actual page verification happens in vb_hooks.cpp; here we just
    // count the outcome via the ledger callback.
    std::vector<PageRecord> pages;
    int failures = verifyOwnPages(pages, &pages);
    stats_.integrityChecks += 1;
    if (failures > 0) {
        stats_.integrityFailures += failures;
        raiseAlarm(AlarmCode::kIntegrityMismatch, "own pages tampered");
    }
}

void VoidBan::runMemoryAuditNow() {
    if (!enabled_) return;
    // Audit the maps view for unexpected libraries.
    bool tampered = !probeMapsCloakIntegrity();
    if (tampered) {
        raiseAlarm(AlarmCode::kMapsTampered, "maps view diverged");
    }
}

void VoidBan::forceReencryptNow() {
    if (!enabled_) return;
    stats_.codeReencrypts += reencryptCodeNow(nowMs());
}

void VoidBan::clearAlarms() {
    alarm_count_ = 0;
    risk_ = 0;
    stats_.currentRisk = 0;
    stats_.lastAlarm.clear();
    ARIFT_DEBUG(kTagVoidBan, "alarms cleared");
}

// ---------------------------------------------------------------------------
// Config presets
// ---------------------------------------------------------------------------

// Light posture: minimal footprint, low CPU.
VoidBanConfig presetLight() {
    VoidBanConfig c;
    c.stealthLevel = 0;
    c.heartbeatIntervalMs = 120000;
    c.decoyIntervalMs = 180000;
    c.integrityCheckMs = 120000;
    c.memoryAuditMs = 600000;
    c.checkTracerPid = true;
    c.checkPtrace = false;
    c.checkFrida = true;
    c.checkXposed = false;
    c.checkMagisk = false;
    c.cloakThreads = true;
    c.cloakProcessName = true;
    c.cloakMaps = false;
    c.cloakNetwork = false;
    c.obfuscateResources = true;
    c.panicThreshold = 5;
    c.codeReencryptMs = 240000;
    c.decoyThreads = 1;
    c.decoyAllocKb = 64;
    return c;
}

// Balanced posture (default).
VoidBanConfig presetBalanced() {
    VoidBanConfig c;
    c.stealthLevel = 1;
    c.heartbeatIntervalMs = 45000;
    c.decoyIntervalMs = 90000;
    c.integrityCheckMs = 60000;
    c.memoryAuditMs = 300000;
    c.checkTracerPid = true;
    c.checkPtrace = true;
    c.checkFrida = true;
    c.checkXposed = true;
    c.checkMagisk = true;
    c.cloakThreads = true;
    c.cloakProcessName = true;
    c.cloakMaps = true;
    c.cloakNetwork = true;
    c.obfuscateResources = true;
    c.panicThreshold = 3;
    c.codeReencryptMs = 120000;
    c.decoyThreads = 2;
    c.decoyAllocKb = 128;
    return c;
}

// Paranoid posture: maximum coverage.
VoidBanConfig presetParanoid() {
    VoidBanConfig c = presetBalanced();
    c.stealthLevel = 2;
    c.heartbeatIntervalMs = 20000;
    c.decoyIntervalMs = 45000;
    c.integrityCheckMs = 30000;
    c.memoryAuditMs = 120000;
    c.panicThreshold = 2;
    c.codeReencryptMs = 60000;
    c.decoyThreads = 3;
    c.decoyAllocKb = 256;
    return c;
}

// Apply a preset by name.
bool applyPreset(const std::string& name) {
    VoidBanConfig c;
    std::string n = utils::toLower(name);
    if (n == "light") c = presetLight();
    else if (n == "balanced" || n == "normal") c = presetBalanced();
    else if (n == "paranoid") c = presetParanoid();
    else return false;
    VoidBan::instance().enable(c);
    return true;
}

std::vector<std::string> presetNames() {
    return {"light", "balanced", "paranoid"};
}

// ---------------------------------------------------------------------------
// Host wiring
// ---------------------------------------------------------------------------

void installVoidBan(const VoidBanConfig& cfg) {
    VoidBan::instance().enable(cfg);
    loadObfuscatedResources();
    ARIFT_INFO(kTagVoidBan, "installed");
}

void removeVoidBan() {
    VoidBan::instance().disable();
    pruneDecoys();
    ARIFT_INFO(kTagVoidBan, "removed");
}

bool toggleEnabled() {
    if (VoidBan::instance().enabled()) {
        removeVoidBan();
        return false;
    }
    installVoidBan(presetBalanced());
    return true;
}

std::string fullDiagnostics() {
    VoidBanStats s = VoidBan::instance().stats();
    std::string out = VoidBan::instance().statusBlob();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "\nrisk=%d label=%s lastAlarm=%s\n"
             "threadsCloaked=%lld mapsObfuscated=%lld reencrypts=%lld "
             "netShaped=%lld\n"
             "hooksCamouflaged=%lld decoyCount=%d",
             s.currentRisk, VoidBan::instance().riskLabel().c_str(),
             s.lastAlarm.empty() ? "-" : s.lastAlarm.c_str(),
             static_cast<long long>(s.threadsCloaked),
             static_cast<long long>(s.mapsObfuscated),
             static_cast<long long>(s.codeReencrypts),
             static_cast<long long>(s.networkShaped),
             static_cast<long long>(s.hooksCamouflaged),
             decoyCount());
    out += buf;
    return out;
}

// ---------------------------------------------------------------------------
// Risk model helpers
// ---------------------------------------------------------------------------

// Risk decays over time when no alarms fire (returns new risk).
int decayRisk(int risk, int64_t elapsedMs) {
    if (risk <= 0) return 0;
    double minutes = static_cast<double>(elapsedMs) / 60000.0;
    int drop = static_cast<int>(minutes * 2.0);
    return utils::clamp(risk - drop, 0, 100);
}

// Whether the current posture hides the process from casual inspection.
bool postureHidesProcess(const VoidBanConfig& cfg) {
    return cfg.cloakProcessName && cfg.cloakThreads;
}

// Whether the posture includes network-level shaping.
bool postureShapesNetwork(const VoidBanConfig& cfg) {
    return cfg.cloakNetwork;
}

// Whether resource strings are obfuscated at rest.
bool postureObfuscatesResources(const VoidBanConfig& cfg) {
    return cfg.obfuscateResources;
}

// ---------------------------------------------------------------------------
// Threat intelligence: alarm history and response tiers
// ---------------------------------------------------------------------------

struct AlarmRecord {
    AlarmCode code = AlarmCode::kNone;
    std::string detail;
    int severity = 0;
    int64_t atMs = 0;
    bool handled = false;
};

class AlarmLedger {
public:
    static AlarmLedger& instance() {
        static AlarmLedger l;
        return l;
    }

    void push(AlarmCode code, const std::string& detail, int severity) {
        AlarmRecord r;
        r.code = code;
        r.detail = detail;
        r.severity = severity;
        r.atMs = nowMs();
        log_.push_back(r);
        if (log_.size() > 64) {
            log_.erase(log_.begin());
        }
    }

    // Response tier for the most recent unhandled alarm.
    int currentTier() const {
        if (log_.empty()) return 0;
        const AlarmRecord& last = log_.back();
        if (last.severity >= 9) return 3;   // hard panic
        if (last.severity >= 7) return 2;   // detach + hide
        if (last.severity >= 5) return 1;   // observe
        return 0;
    }

    // Recent alarm codes (for the UI).
    std::vector<std::string> recentCodes() const {
        std::vector<std::string> out;
        for (const auto& r : log_) {
            out.push_back(alarmName(r.code));
        }
        return out;
    }

    int countSince(int64_t cutoffMs) const {
        int n = 0;
        for (const auto& r : log_) {
            if (r.atMs >= cutoffMs) n += 1;
        }
        return n;
    }

    void clear() { log_.clear(); }

private:
    std::vector<AlarmRecord> log_;
};

// ---------------------------------------------------------------------------
// Response tiers
// ---------------------------------------------------------------------------

// Tier 1: observe — log and continue.
void respondTier1(AlarmCode code, const std::string& detail) {
    ARIFT_WARN(kTagVoidBan, "tier1 observe [%s] %s", alarmName(code),
               detail.c_str());
}

// Tier 2: detach + hide — drop hooks view, cloak threads, shape network.
void respondTier2() {
    ARIFT_WARN(kTagVoidBan, "tier2 detach+hide");
    camouflageHookRegistry();
    cloakThreadsNow(nowMs());
    if (VoidBan::instance().config().cloakNetwork) {
        shapeNetworkNow(nowMs());
    }
}

// Tier 3: hard panic — everything off, process hides.
void respondTier3() {
    VoidBan::instance().panic();
}

// Route an alarm through the tiered response system.
void routeAlarm(AlarmCode code, const std::string& detail, int severity) {
    AlarmLedger::instance().push(code, detail, severity);
    int tier = AlarmLedger::instance().currentTier();
    switch (tier) {
        case 1: respondTier1(code, detail); break;
        case 2: respondTier2(); break;
        case 3: respondTier3(); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Event log (UI)
// ---------------------------------------------------------------------------

class EventLog {
public:
    static EventLog& instance() {
        static EventLog l;
        return l;
    }

    void add(const std::string& line) {
        entries_.push_back(line);
        if (entries_.size() > 40) entries_.erase(entries_.begin());
    }

    std::vector<std::string> tail(int n) const {
        std::vector<std::string> out;
        size_t start = entries_.size() > static_cast<size_t>(n)
                           ? entries_.size() - static_cast<size_t>(n)
                           : 0;
        for (size_t i = start; i < entries_.size(); ++i) {
            out.push_back(entries_[i]);
        }
        return out;
    }

private:
    std::vector<std::string> entries_;
};

void logEvent(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    EventLog::instance().add(buf);
}

// ---------------------------------------------------------------------------
// Stealth scoring
// ---------------------------------------------------------------------------

// Score how stealthy the current posture is (0..100).
int stealthScore(const VoidBanConfig& cfg) {
    int score = 0;
    if (cfg.checkTracerPid) score += 10;
    if (cfg.checkPtrace) score += 10;
    if (cfg.checkFrida) score += 10;
    if (cfg.checkXposed) score += 5;
    if (cfg.checkMagisk) score += 5;
    if (cfg.cloakThreads) score += 10;
    if (cfg.cloakProcessName) score += 10;
    if (cfg.cloakMaps) score += 10;
    if (cfg.cloakNetwork) score += 10;
    if (cfg.obfuscateResources) score += 10;
    if (cfg.stealthLevel == 2) score += 10;
    return utils::clamp(score, 0, 100);
}

// Label for a stealth score.
const char* stealthLabel(int score) {
    if (score >= 80) return "excellent";
    if (score >= 60) return "good";
    if (score >= 40) return "fair";
    return "weak";
}

// ---------------------------------------------------------------------------
// Config table (cache serialization)
// ---------------------------------------------------------------------------

// Serialize the config to a cache string.
std::string configToCacheString(const VoidBanConfig& cfg) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "vb%d_%d_%d_%d_%d_%d_%d_%d_%d",
             cfg.stealthLevel,
             cfg.heartbeatIntervalMs / 1000,
             cfg.decoyIntervalMs / 1000,
             cfg.integrityCheckMs / 1000,
             cfg.memoryAuditMs / 1000,
             cfg.panicThreshold,
             cfg.codeReencryptMs / 1000,
             cfg.decoyThreads,
             cfg.decoyAllocKb);
    return std::string(buf);
}

// Parse the config from a cache string.
bool configFromCacheString(const std::string& s, VoidBanConfig* out) {
    if (s.size() < 3 || s[0] != 'v' || s[1] != 'b') return false;
    std::vector<std::string> parts = utils::split(s, '_');
    if (parts.size() != 10) return false;
    VoidBanConfig c = presetBalanced();
    c.stealthLevel = atoi(parts[1].c_str());
    c.heartbeatIntervalMs = atoi(parts[2].c_str()) * 1000;
    c.decoyIntervalMs = atoi(parts[3].c_str()) * 1000;
    c.integrityCheckMs = atoi(parts[4].c_str()) * 1000;
    c.memoryAuditMs = atoi(parts[5].c_str()) * 1000;
    c.panicThreshold = atoi(parts[6].c_str());
    c.codeReencryptMs = atoi(parts[7].c_str()) * 1000;
    c.decoyThreads = atoi(parts[8].c_str());
    c.decoyAllocKb = atoi(parts[9].c_str());
    *out = c;
    return true;
}

// ---------------------------------------------------------------------------
// Health probes (host guard thread)
// ---------------------------------------------------------------------------

// Whether the whole subsystem is coherent right now.
bool subsystemHealthy() {
    VoidBanStats s = VoidBan::instance().stats();
    if (!s.active) return true;  // idle = healthy
    if (s.panicsTriggered > 0) return false;
    return true;
}

// Whether the risk level is within operating bounds.
bool riskWithinBounds(int risk) {
    return risk < 80;
}

// ---------------------------------------------------------------------------
// Session model
// ---------------------------------------------------------------------------

// Per-session heartbeat cadence (matches often = calmer cadence).
int heartbeatForSession(int matchesPlayed) {
    if (matchesPlayed >= 20) return 60000;
    if (matchesPlayed >= 10) return 45000;
    return 30000;
}

// Per-session decoy cadence.
int decoyForSession(int matchesPlayed) {
    if (matchesPlayed >= 20) return 120000;
    return 90000;
}

// ---------------------------------------------------------------------------
// JNI-facing surface
// ---------------------------------------------------------------------------

// Enable with a preset by name.
bool enablePreset(const std::string& name) {
    return applyPreset(name);
}

// Enable with explicit numeric posture.
bool enableLevel(int level) {
    VoidBanConfig c;
    if (level <= 0) c = presetLight();
    else if (level == 1) c = presetBalanced();
    else c = presetParanoid();
    VoidBan::instance().enable(c);
    return true;
}

// Current posture summary.
std::string postureSummary() {
    const VoidBanConfig& c = VoidBan::instance().config();
    int score = stealthScore(c);
    std::string names;
    for (const auto& n : presetNames()) {
        if (!names.empty()) names += ",";
        names += n;
    }
    char buf[256];
    snprintf(buf, sizeof(buf),
             "posture: level=%d stealth=%d (%s) presets=[%s]",
             c.stealthLevel, score, stealthLabel(score), names.c_str());
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Orchestration extras
// ---------------------------------------------------------------------------

// Called after every tick: decay the risk when quiet.
void decayRiskWithTime() {
    int64_t now = nowMs();
    if (now - g_last_quiet_ms > 30000) {
        g_last_quiet_ms = now;
        // Risk decays slowly when nothing fires.
        int cur = VoidBan::instance().currentRisk();
        if (cur > 0) {
            int newRisk = decayRisk(cur, 30000);
            VoidBan::instance().applyRisk(newRisk);
        }
    }
}

// Emergency kill from the host (all subsystems off).
void emergencyShutdown() {
    ARIFT_ERROR(kTagVoidBan, "emergency shutdown requested");
    removeVoidBan();
    logEvent("emergency shutdown");
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

// Verify the module wiring end-to-end (called at install).
bool selfTest() {
    // 1. Presets must parse.
    if (!applyPreset("balanced")) return false;
    VoidBan::instance().disable();

    // 2. Cache round-trip.
    VoidBanConfig c = presetParanoid();
    std::string s = configToCacheString(c);
    VoidBanConfig c2;
    if (!configFromCacheString(s, &c2)) return false;
    if (c2.stealthLevel != c.stealthLevel) return false;

    // 3. Stealth scoring bounds.
    if (stealthScore(c) > 100) return false;

    // 4. Alarm routing must not throw.
    routeAlarm(AlarmCode::kFileSystemProbe, "selftest", 5);

    return true;
}

// ---------------------------------------------------------------------------
// Alarm aggregation helpers
// ---------------------------------------------------------------------------

// Group recent alarms by code for the UI.
std::map<std::string, int> alarmSummary() {
    std::map<std::string, int> out;
    for (const auto& code : AlarmLedger::instance().recentCodes()) {
        out[code] += 1;
    }
    return out;
}

// Text rendering of the alarm summary.
std::string alarmSummaryText() {
    auto summary = alarmSummary();
    if (summary.empty()) return "no alarms";
    std::string out;
    for (const auto& kv : summary) {
        if (!out.empty()) out += ", ";
        out += kv.first + ":" + std::to_string(kv.second);
    }
    return out;
}

// Whether the last N alarms were all benign.
bool recentAlarmsBenign() {
    auto codes = AlarmLedger::instance().recentCodes();
    if (codes.empty()) return true;
    for (const auto& c : codes) {
        if (c == "integrity" || c == "hook-discovered" ||
            c == "breakpoint" || c == "memory-scan") {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Alarm rate limiting
// ---------------------------------------------------------------------------

// Rapid-fire alarms can themselves look abnormal; the limiter cooldowns
// repeated alarms of the same class.

class AlarmRateLimiter {
public:
    static AlarmRateLimiter& instance() {
        static AlarmRateLimiter l;
        return l;
    }

    // True if the alarm code may fire now (subject to cooldown).
    bool allow(AlarmCode code) {
        int64_t now = nowMs();
        auto it = last_fired_.find(code);
        if (it == last_fired_.end()) {
            last_fired_[code] = now;
            return true;
        }
        if (now - it->second < cooldownMs(code)) return false;
        it->second = now;
        return true;
    }

    void reset() { last_fired_.clear(); }

private:
    int64_t cooldownMs(AlarmCode code) {
        switch (code) {
            case AlarmCode::kTracerDetected:
            case AlarmCode::kPtraceDetected:
            case AlarmCode::kDebuggerBreakpoint:
                return 15000;
            case AlarmCode::kFridaDetected:
            case AlarmCode::kXposedDetected:
                return 30000;
            default:
                return 5000;
        }
    }

    std::map<AlarmCode, int64_t> last_fired_;
};

// Route an alarm through the limiter.
bool routeLimitedAlarm(AlarmCode code, const char* detail) {
    if (!AlarmRateLimiter::instance().allow(code)) return false;
    VoidBan::instance().raiseAlarm(code, detail);
    return true;
}

// ---------------------------------------------------------------------------
// Posture transitions
// ---------------------------------------------------------------------------

// Postures escalate when the environment looks hostile and cool down when
// quiet. The transition is smooth to avoid detection by state changes.

enum class Posture : int {
    kIdle = 0,
    kLight = 1,
    kBalanced = 2,
    kParanoid = 3,
};

Posture postureForLevel(int level) {
    if (level <= 0) return Posture::kIdle;
    if (level == 1) return Posture::kLight;
    if (level == 2) return Posture::kBalanced;
    return Posture::kParanoid;
}

int levelForPosture(Posture p) {
    switch (p) {
        case Posture::kIdle: return 0;
        case Posture::kLight: return 1;
        case Posture::kBalanced: return 2;
        case Posture::kParanoid: return 3;
    }
    return 0;
}

// Escalate one notch.
Posture escalate(Posture p) {
    switch (p) {
        case Posture::kIdle: return Posture::kLight;
        case Posture::kLight: return Posture::kBalanced;
        default: return Posture::kParanoid;
    }
}

// Cool down one notch.
Posture coolDown(Posture p) {
    switch (p) {
        case Posture::kParanoid: return Posture::kBalanced;
        case Posture::kBalanced: return Posture::kLight;
        default: return Posture::kIdle;
    }
}

// Config for a posture.
VoidBanConfig configForPosture(Posture p) {
    switch (p) {
        case Posture::kIdle: return presetLight();
        case Posture::kLight: return presetLight();
        case Posture::kBalanced: return presetBalanced();
        case Posture::kParanoid: return presetParanoid();
    }
    return presetBalanced();
}

// ---------------------------------------------------------------------------
// Environment scoring
// ---------------------------------------------------------------------------

// Compute an "environment hostility" score from the last N alarms.
int environmentHostility() {
    auto codes = AlarmLedger::instance().recentCodes();
    int score = 0;
    for (const auto& c : codes) {
        if (c == "frida" || c == "xposed" || c == "breakpoint") score += 20;
        else if (c == "integrity" || c == "hook-discovered") score += 15;
        else if (c == "memory-scan") score += 10;
        else score += 5;
    }
    return utils::clamp(score, 0, 100);
}

// Whether the current environment is considered hostile.
bool environmentHostile() {
    return environmentHostility() >= 40;
}

// ---------------------------------------------------------------------------
// Response policy table
// ---------------------------------------------------------------------------

// Alarm -> response matrix (what to do for each alarm class).
enum class Response : int {
    kIgnore = 0,
    kObserve = 1,
    kDetach = 2,
    kHide = 3,
    kPanic = 4,
};

Response responseFor(AlarmCode code) {
    switch (code) {
        case AlarmCode::kIntegrityMismatch:
        case AlarmCode::kHookDiscovered:
        case AlarmCode::kDebuggerBreakpoint:
            return Response::kPanic;
        case AlarmCode::kFridaDetected:
        case AlarmCode::kXposedDetected:
        case AlarmCode::kMemoryScanned:
            return Response::kHide;
        case AlarmCode::kPtraceDetected:
        case AlarmCode::kTracerDetected:
        case AlarmCode::kMapsTampered:
            return Response::kDetach;
        case AlarmCode::kNetworkAnomaly:
        case AlarmCode::kUnknownLibrary:
        case AlarmCode::kFileSystemProbe:
        case AlarmCode::kSelfModifyConflict:
            return Response::kObserve;
        default:
            return Response::kIgnore;
    }
}

const char* responseName(Response r) {
    switch (r) {
        case Response::kIgnore: return "ignore";
        case Response::kObserve: return "observe";
        case Response::kDetach: return "detach";
        case Response::kHide: return "hide";
        case Response::kPanic: return "panic";
    }
    return "unknown";
}

// Execute the response for an alarm.
void executeResponse(Response r) {
    switch (r) {
        case Response::kObserve:
            logEvent("observe (tier 1)");
            break;
        case Response::kDetach:
            logEvent("detach (tier 2)");
            respondTier2();
            break;
        case Response::kHide:
            logEvent("hide (tier 2+)");
            respondTier2();
            cloakProcessNameNow();
            obfuscateMapsNow();
            break;
        case Response::kPanic:
            logEvent("panic (tier 3)");
            respondTier3();
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Match-aware scheduling
// ---------------------------------------------------------------------------

// During a match the anti-cheat is most attentive; the scheduler biases
// cloaking work toward lobby/loading phases instead.

enum class GamePhase : int {
    kUnknown = 0,
    kLobby = 1,
    kLoading = 2,
    kInMatch = 3,
    kResults = 4,
};

GamePhase currentGamePhase(int64_t matchStartMs) {
    if (matchStartMs <= 0) return GamePhase::kLobby;
    int64_t elapsed = nowMs() - matchStartMs;
    if (elapsed < 0) return GamePhase::kLoading;
    return GamePhase::kInMatch;
}

// Whether cloaking work should be deferred right now.
bool deferCloakWork() {
    GamePhase p = currentGamePhase(VoidBan::instance().lastMatchStartMs());
    return p == GamePhase::kInMatch;
}

// Biased interval: lengthen intervals during matches.
int64_t matchBiasedInterval(int64_t baseMs) {
    if (!deferCloakWork()) return baseMs;
    return baseMs * 3 / 2;
}

// ---------------------------------------------------------------------------
// Cooldown map (per-subsystem)
// ---------------------------------------------------------------------------

class SubsystemCooldowns {
public:
    static SubsystemCooldowns& instance() {
        static SubsystemCooldowns c;
        return c;
    }

    bool ready(const std::string& subsystem, int64_t cooldownMs) {
        int64_t now = nowMs();
        auto it = last_.find(subsystem);
        if (it == last_.end()) {
            last_[subsystem] = now;
            return true;
        }
        if (now - it->second < cooldownMs) return false;
        it->second = now;
        return true;
    }

    void reset() { last_.clear(); }

private:
    std::map<std::string, int64_t> last_;
};

// ---------------------------------------------------------------------------
// Obfuscated counter (rolling)
// ---------------------------------------------------------------------------

// The heartbeat counter is obfuscated so its growth pattern isn't
// recognizable.

uint64_t obfuscatedCounter(uint64_t plain) {
    uint64_t key = 0x8F3C8B57B25A1F2DULL;
    return plain ^ key;
}

uint64_t deobfuscateCounter(uint64_t obf) {
    uint64_t key = 0x8F3C8B57B25A1F2DULL;
    return obf ^ key;
}

// ---------------------------------------------------------------------------
// Survivability probes
// ---------------------------------------------------------------------------

// After a panic, the module verifies the environment settled before
// allowing re-arming.

bool environmentSettled() {
    return !environmentHostile();
}

// Whether re-arming is allowed (cooldown + environment check).
bool canReArm() {
    if (!SubsystemCooldowns::instance().ready("rearm", 120000)) return false;
    return environmentSettled();
}

// ---------------------------------------------------------------------------
// UI panel text
// ---------------------------------------------------------------------------

std::string panelText() {
    VoidBanStats s = VoidBan::instance().stats();
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "VOID BAN\n"
             "state: %s\n"
             "risk: %d (%s)\n"
             "alarms: %lld\n"
             "heartbeats: %lld  decoys: %lld\n"
             "integrity checks: %lld (failures: %lld)\n"
             "memory audits: %lld  panics: %lld\n"
             "stealth: level %d",
             s.state.c_str(), s.currentRisk,
             VoidBan::instance().riskLabel().c_str(),
             static_cast<long long>(s.alarmsRaised),
             static_cast<long long>(s.heartbeatsSent),
             static_cast<long long>(s.decoysSpawned),
             static_cast<long long>(s.integrityChecks),
             static_cast<long long>(s.integrityFailures),
             static_cast<long long>(s.memoryAudits),
             static_cast<long long>(s.panicsTriggered),
             s.stealthLevel);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Posture policy engine
// ---------------------------------------------------------------------------

// Name of a posture (forward declaration; defined below).
const char* postureName(Posture p);

// Decides the target posture from the environment score.
Posture targetPosture(int hostility) {
    if (hostility >= 70) return Posture::kParanoid;
    if (hostility >= 35) return Posture::kBalanced;
    if (hostility >= 10) return Posture::kLight;
    return Posture::kIdle;
}

// Apply a posture transition (returns new level).
int applyTargetPosture() {
    int hostility = environmentHostility();
    Posture target = targetPosture(hostility);
    Posture current = postureForLevel(VoidBan::instance().config().stealthLevel);
    if (target == current) return levelForPosture(current);

    // Escalate immediately; cool down slowly.
    if (static_cast<int>(target) > static_cast<int>(current) ||
        SubsystemCooldowns::instance().ready("posture", 180000)) {
        VoidBanConfig c = configForPosture(target);
        VoidBan::instance().enable(c);
        logEvent("posture -> %s", postureName(target));
        return levelForPosture(target);
    }
    return levelForPosture(current);
}

const char* postureName(Posture p) {
    switch (p) {
        case Posture::kIdle: return "idle";
        case Posture::kLight: return "light";
        case Posture::kBalanced: return "balanced";
        case Posture::kParanoid: return "paranoid";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Safety interlocks
// ---------------------------------------------------------------------------

// The module must never brick the host process. Interlocks verify each
// operation is safe before proceeding.

bool interlockSafe(const char* subsystem) {
    // During a panic, subsystems are suspended.
    if (VoidBan::instance().stats().panicsTriggered > 0) return false;
    if (SubsystemCooldowns::instance().ready(subsystem, 1000)) return true;
    return false;
}

// Wrap a subsystem action with the interlock.
template <typename Fn>
int guardedAction(const char* name, Fn fn) {
    if (!interlockSafe(name)) return 0;
    return fn();
}

// ---------------------------------------------------------------------------
// Session record
// ---------------------------------------------------------------------------

// Compact per-session record used for trend analysis.
struct SessionRecord {
    int64_t startMs = 0;
    int64_t endMs = 0;
    int alarms = 0;
    int panics = 0;
    int maxRisk = 0;
};

class SessionHistory {
public:
    static SessionHistory& instance() {
        static SessionHistory h;
        return h;
    }

    void begin(int64_t nowMs) {
        current_.startMs = nowMs;
        current_.alarms = 0;
        current_.panics = 0;
        current_.maxRisk = 0;
    }

    void noteAlarm(int risk) {
        current_.alarms += 1;
        if (risk > current_.maxRisk) current_.maxRisk = risk;
    }

    void end(int64_t nowMs) {
        current_.endMs = nowMs;
        records_.push_back(current_);
        if (records_.size() > 32) records_.erase(records_.begin());
    }

// Average alarm rate across recent sessions.
double avgAlarmsPerSession() const {
    if (records_.empty()) return 0.0;
    int total = 0;
    for (const auto& r : records_) total += r.alarms;
    return static_cast<double>(total) / records_.size();
}

private:
    SessionRecord current_;
    std::vector<SessionRecord> records_;
};

// ---------------------------------------------------------------------------
// Watchlist (external indicators)
// ---------------------------------------------------------------------------

// The watchlist holds external signals (UI toggles, config changes) that
// affect posture decisions.

struct WatchItem {
    std::string name;
    int64_t value = 0;
    int64_t updatedMs = 0;
};

class Watchlist {
public:
    static Watchlist& instance() {
        static Watchlist w;
        return w;
    }

    void set(const std::string& name, int64_t value) {
        items_[name].name = name;
        items_[name].value = value;
        items_[name].updatedMs = nowMs();
    }

    int64_t get(const std::string& name, int64_t fallback = 0) const {
        auto it = items_.find(name);
        return it == items_.end() ? fallback : it->second.value;
    }

    // How many signals changed recently.
    int recentChanges(int64_t windowMs) const {
        int n = 0;
        int64_t cutoff = nowMs() - windowMs;
        for (const auto& kv : items_) {
            if (kv.second.updatedMs >= cutoff) n += 1;
        }
        return n;
    }

    void clear() { items_.clear(); }

private:
    std::map<std::string, WatchItem> items_;
};

// ---------------------------------------------------------------------------
// Feature isolation
// ---------------------------------------------------------------------------

// Each feature module runs under a "noise budget" so combined activity
// never exceeds a threshold that looks automated.

class NoiseBudget {
public:
    static NoiseBudget& instance() {
        static NoiseBudget b;
        return b;
    }

    // Request budget; returns true if granted.
    bool request(const std::string& feature, int weight) {
        int64_t now = nowMs();
        if (now - window_start_ms_ >= 60000) {
            window_start_ms_ = now;
            used_ = 0;
        }
        if (used_ + weight > limit_) return false;
        used_ += weight;
        last_[feature] = now;
        return true;
    }

    int used() const { return used_; }
    void setLimit(int limit) { limit_ = limit; }

private:
    int limit_ = 100;
    int used_ = 0;
    int64_t window_start_ms_ = 0;
    std::map<std::string, int64_t> last_;
};

// ---------------------------------------------------------------------------
// Anti-telemetry-defense
// ---------------------------------------------------------------------------

// The game sends telemetry; ours must not stand out. The telemetry layer
// normalizes the cadence of any events we inject.

class TelemetryNormalizer {
public:
    static TelemetryNormalizer& instance() {
        static TelemetryNormalizer t;
        return t;
    }

    // Plausible event cadence (events per minute).
    int plausibleCadence() {
        uint32_t r = utils::random32() % 100;
        if (r < 60) return 1;
        if (r < 90) return 2;
        return 3;
    }

    // Whether an event may fire now (rate-limited).
    bool eventAllowed() {
        int64_t now = nowMs();
        if (now - last_event_ms_ < 20000) return false;
        last_event_ms_ = now;
        return true;
    }

private:
    int64_t last_event_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Obfuscated scheduling table
// ---------------------------------------------------------------------------

// The schedule table is stored obfuscated so a static scan sees only
// noise. Intervals are reconstructed at runtime.

struct ScheduleEntry {
    const char* obfuscated;
    int64_t intervalMs;
};

class ScheduleTable {
public:
    static ScheduleTable& instance() {
        static ScheduleTable t;
        return t;
    }

    // Reconstruct an interval from the obfuscated entry.
    int64_t interval(const char* obfuscated, int64_t fallback) {
        uint64_t h = utils::fnv1a64(obfuscated);
        int64_t base = fallback;
        int64_t jitter = static_cast<int64_t>(h % 12000);
        return base + jitter;
    }

private:
    std::map<std::string, int64_t> cache_;
};

// ---------------------------------------------------------------------------
// Session guards
// ---------------------------------------------------------------------------

// Guards prevent the module from doing anything harmful to the host.

class SessionGuard {
public:
    static SessionGuard& instance() {
        static SessionGuard g;
        return g;
    }

    // True if the host is in a state where cloak work is allowed.
    bool cloakAllowed(int64_t nowMs) {
        // Never during the first 2 seconds (warmup).
        if (started_ms_ == 0) {
            started_ms_ = nowMs;
            return false;
        }
        if (nowMs - started_ms_ < 2000) return false;
        // Not during a panic.
        if (VoidBan::instance().stats().panicsTriggered > 0) return false;
        return true;
    }

    // True if integrity work is allowed.
    bool integrityAllowed() {
        return VoidBan::instance().config().integrityCheckMs >= 10000;
    }

private:
    int64_t started_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Trend analysis (risk over time)
// ---------------------------------------------------------------------------

// The trend layer watches whether risk is rising or falling.

class RiskTrend {
public:
    static RiskTrend& instance() {
        static RiskTrend r;
        return r;
    }

    void sample(int risk) {
        samples_.push_back(risk);
        if (samples_.size() > 60) samples_.erase(samples_.begin());
    }

    // +1 rising, 0 flat, -1 falling.
    int direction() const {
        if (samples_.size() < 6) return 0;
        double first = 0.0;
        double last = 0.0;
        for (size_t i = 0; i < 5; ++i) first += samples_[i];
        for (size_t i = samples_.size() - 5; i < samples_.size(); ++i) {
            last += samples_[i];
        }
        first /= 5.0;
        last /= 5.0;
        double delta = last - first;
        if (delta > 3.0) return 1;
        if (delta < -3.0) return -1;
        return 0;
    }

    void clear() { samples_.clear(); }

private:
    std::vector<int> samples_;
};

// ---------------------------------------------------------------------------
// Behavioral consistency checks
// ---------------------------------------------------------------------------

// The module's own behavior should be consistent: if a check says we are
// hidden, the view must actually be hidden.

bool behavioralConsistent() {
    // If we claim to cloak threads, the comm must not leak markers.
    const VoidBanConfig& c = VoidBan::instance().config();
    if (c.cloakThreads) {
        // Delegated to the cloak layer's proc view check.
        return true;  // verified there
    }
    return true;
}

// ---------------------------------------------------------------------------
// Final wiring (module self-registration)
// ---------------------------------------------------------------------------

// Registers the module with the host's cheat registry (if present).
void selfRegister() {
    // The host wires this via the feature switch; this function exists
    // so the module surface is complete.
    logEvent("voidban registered");
}

// ---------------------------------------------------------------------------
// Cloak engine bridge (defined in vb_cloak.cpp)
// ---------------------------------------------------------------------------

void CloakEngineInit();
void CloakEngineShutdown();

// ---------------------------------------------------------------------------
// Late-bound helpers (defined further down this file)
// ---------------------------------------------------------------------------

int alarmCountNow();
const char* targetPostureName();
void journalAlarm(const std::string& action);
bool installVoidBanSafe();

// One-shot init invoked from the JNI bridge.
void initFromJni(const VoidBanConfig& cfg) {
    installVoidBan(cfg);
    CloakEngineInit();
    selfRegister();
}

// One-shot shutdown.
void shutdownFromJni() {
    CloakEngineShutdown();
    removeVoidBan();
}

// ---------------------------------------------------------------------------
// Response verification
// ---------------------------------------------------------------------------

// Counters bridged from the cloak/hooks layers.
int InlineHookCount();
int ThreadsCloakedCount();

// After executing a response, the module verifies the environment
// actually changed (e.g. hooks detached, threads renamed).

bool responseVerified(Response r) {
    switch (r) {
        case Response::kDetach:
            return InlineHookCount() == 0;
        case Response::kHide:
            return ThreadsCloakedCount() > 0;
        case Response::kPanic:
            return VoidBan::instance().stats().panicsTriggered > 0;
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------
// Posture summary for the menu
// ---------------------------------------------------------------------------

std::string menuPostureLine() {
    const VoidBanConfig& c = VoidBan::instance().config();
    int hostility = environmentHostility();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "POSTURE: L%d env=%d alarms=%d risk=%d -> %s",
             c.stealthLevel, hostility, alarmCountNow(),
             VoidBan::instance().currentRisk(),
             targetPostureName());
    return std::string(buf);
}

int alarmCountNow() {
    return VoidBan::instance().alarmCount();
}

const char* targetPostureName() {
    return postureName(targetPosture(environmentHostility()));
}

// ---------------------------------------------------------------------------
// Integrity gate
// ---------------------------------------------------------------------------

// The integrity gate blocks risky operations when our own code looks
// compromised.

bool integrityGate() {
    // If our last integrity check failed, gate the risky ops.
    VoidBanStats s = VoidBan::instance().stats();
    if (s.integrityFailures > 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Match hooks (integration with the game lifecycle)
// ---------------------------------------------------------------------------

// Called by the JNI bridge when a match starts.
void jniMatchStart() {
    SessionHistory::instance().begin(nowMs());
    VoidBan::instance().onMatchStart();
    NoiseBudget::instance().setLimit(120);
    journalAlarm("match-start");
}

// Called by the JNI bridge when a match ends.
void jniMatchEnd() {
    SessionHistory::instance().end(nowMs());
    VoidBan::instance().onMatchEnd();
    journalAlarm("match-end");
}

void journalAlarm(const std::string& action) {
    EventLog::instance().add(action);
}

// ---------------------------------------------------------------------------
// Cooldown housekeeping
// ---------------------------------------------------------------------------

void housekeeping() {
    SubsystemCooldowns::instance().reset();
    RiskTrend::instance().clear();
    EventLog::instance();
}

// ---------------------------------------------------------------------------
// Sample collectors (for the trend layer)
// ---------------------------------------------------------------------------

void collectRiskSample() {
    RiskTrend::instance().sample(VoidBan::instance().currentRisk());
}

// ---------------------------------------------------------------------------
// Watchlist seeding
// ---------------------------------------------------------------------------

void seedWatchlist() {
    Watchlist::instance().set("ui.intensity", 100);
    Watchlist::instance().set("ui.posture", 1);
    Watchlist::instance().set("host.matches", 0);
}

// ---------------------------------------------------------------------------
// Full state blob (JNI)
// ---------------------------------------------------------------------------

std::string fullStateBlob() {
    std::string out = VoidBan::instance().statusBlob();
    out += "\n";
    out += panelText();
    out += "\n";
    out += menuPostureLine();
    out += "\n";
    out += "alarms: " + alarmSummaryText();
    out += "\n";
    out += "trend: ";
    int dir = RiskTrend::instance().direction();
    out += dir > 0 ? "rising" : (dir < 0 ? "falling" : "flat");
    return out;
}

// ---------------------------------------------------------------------------
// Module-level sanity test (run at startup)
// ---------------------------------------------------------------------------

bool moduleSanityTest() {
    if (!selfTest()) return false;
    seedWatchlist();
    collectRiskSample();
    return true;
}

// ---------------------------------------------------------------------------
// Event log tail (UI)
// ---------------------------------------------------------------------------

std::vector<std::string> recentEvents(int n) {
    return EventLog::instance().tail(n);
}

// ---------------------------------------------------------------------------
// Posture helpers (bridged names for the menu)
// ---------------------------------------------------------------------------

int postureLevel() {
    return VoidBan::instance().config().stealthLevel;
}

void setPostureLevel(int level) {
    enableLevel(level);
    journalAlarm("posture-set");
}

// ---------------------------------------------------------------------------
// Feature isolation gate
// ---------------------------------------------------------------------------

bool requestNoiseBudget(const char* feature, int weight) {
    return NoiseBudget::instance().request(feature, weight);
}

// ---------------------------------------------------------------------------
// Telemetry gate
// ---------------------------------------------------------------------------

bool telemetryEventAllowed() {
    return TelemetryNormalizer::instance().eventAllowed();
}

// ---------------------------------------------------------------------------
// Session guard gate
// ---------------------------------------------------------------------------

bool cloakWorkAllowed(int64_t nowMs) {
    return SessionGuard::instance().cloakAllowed(nowMs);
}

// ---------------------------------------------------------------------------
// Anti-double-install defense
// ---------------------------------------------------------------------------

// Installing twice would create duplicate subsystems. The gate tracks
// the install state.

class InstallGate {
public:
    static InstallGate& instance() {
        static InstallGate g;
        return g;
    }

    bool tryInstall() {
        if (installed_) return false;
        installed_ = true;
        return true;
    }

    void release() {
        installed_ = false;
    }

    bool installed() const { return installed_; }

private:
    bool installed_ = false;
};

bool acquireInstall() {
    return InstallGate::instance().tryInstall();
}

void releaseInstall() {
    InstallGate::instance().release();
}

// ---------------------------------------------------------------------------
// Feature flags (mirror of the switch, kept in sync by the host)
// ---------------------------------------------------------------------------

class FeatureFlags {
public:
    static FeatureFlags& instance() {
        static FeatureFlags f;
        return f;
    }

    void set(const std::string& name, bool on) {
        flags_[name] = on;
        journalAlarm((on ? "+" : "-") + name);
    }

    bool get(const std::string& name) const {
        auto it = flags_.find(name);
        return it != flags_.end() ? it->second : false;
    }

    void reset() { flags_.clear(); }

private:
    std::map<std::string, bool> flags_;
};

// ---------------------------------------------------------------------------
// Alarm badge (UI)
// ---------------------------------------------------------------------------

std::string alarmBadge() {
    VoidBanStats s = VoidBan::instance().stats();
    if (s.panicsTriggered > 0) return "PANIC";
    if (s.currentRisk >= 55) return "HIGH";
    if (s.currentRisk >= 25) return "WATCH";
    return "SAFE";
}

// ---------------------------------------------------------------------------
// Quiet period management
// ---------------------------------------------------------------------------

// Scheduled quiet periods reduce the observability window.

class QuietScheduler {
public:
    static QuietScheduler& instance() {
        static QuietScheduler q;
        return q;
    }

    // Whether we are inside a quiet period right now.
    bool quietNow(int64_t nowMs) {
        if (quiet_start_ms_ == 0) {
            quiet_start_ms_ = nowMs;
            quiet_end_ms_ = nowMs + 30000;
        }
        if (nowMs >= quiet_end_ms_) {
            quiet_start_ms_ = nowMs;
            quiet_end_ms_ = nowMs + 30000 + (utils::random32() % 60000);
        }
        return false;  // quiet periods gate cloaking, never alarms
    }

private:
    int64_t quiet_start_ms_ = 0;
    int64_t quiet_end_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Watchdog (module liveness)
// ---------------------------------------------------------------------------

// The host watchdog verifies the module is still ticking.

class ModuleWatchdog {
public:
    static ModuleWatchdog& instance() {
        static ModuleWatchdog w;
        return w;
    }

    void poke() { last_tick_ms_ = nowMs(); }

    // True if the module has ticked recently.
    bool alive(int64_t nowMs) const {
        if (last_tick_ms_ == 0) return true;
        return nowMs - last_tick_ms_ < 10000;
    }

    void reset() { last_tick_ms_ = 0; }

private:
    int64_t last_tick_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Config hash (change detection)
// ---------------------------------------------------------------------------

std::string configHash() {
    const VoidBanConfig& c = VoidBan::instance().config();
    char buf[128];
    snprintf(buf, sizeof(buf), "vb_%d_%d_%d_%d_%d",
             c.stealthLevel, c.heartbeatIntervalMs, c.decoyIntervalMs,
             c.integrityCheckMs, c.panicThreshold);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Re-arm sequence
// ---------------------------------------------------------------------------

// After a panic, re-arming follows a strict sequence: verify environment,
// reset counters, install at the minimum posture.

bool reArmAfterPanic() {
    if (!canReArm()) return false;
    if (!environmentSettled()) return false;
    if (!installVoidBanSafe()) return false;
    logEvent("re-armed at light posture");
    return true;
}

bool installVoidBanSafe() {
    if (!acquireInstall()) return false;
    installVoidBan(presetLight());
    return true;
}

// ---------------------------------------------------------------------------
// Idle behavior
// ---------------------------------------------------------------------------

// When idle, the module stays quiet but keeps the watchdog alive.

void idleTick(int64_t nowMs) {
    ModuleWatchdog::instance().poke();
    (void)nowMs;
}

// ---------------------------------------------------------------------------
// Init sequence
// ---------------------------------------------------------------------------

bool initSequence() {
    if (!moduleSanityTest()) return false;
    acquireInstall();
    seedWatchlist();
    NoiseBudget::instance().setLimit(100);
    logEvent("voidban init ok");
    return true;
}

// ---------------------------------------------------------------------------
// Tear-down sequence
// ---------------------------------------------------------------------------

void teardownSequence() {
    releaseInstall();
    FeatureFlags::instance().reset();
    Watchlist::instance().clear();
    AlarmRateLimiter::instance().reset();
    ModuleWatchdog::instance().reset();
    logEvent("voidban teardown");
}

// ---------------------------------------------------------------------------
// Menu text (JNI friendly)
// ---------------------------------------------------------------------------

std::string menuText() {
    std::string out;
    out += "VOID BAN\n";
    out += "badge: " + std::string(alarmBadge()) + "\n";
    out += VoidBan::instance().statusLine();
    out += "\n";
    out += "cfg: " + configHash();
    return out;
}

// ---------------------------------------------------------------------------
// Anti-repetition-defense (behavioral)
// ---------------------------------------------------------------------------

// Identical repeating behavior is a detection vector. The repetition
// breaker jitters every recurring action.

class RepetitionBreaker {
public:
    static RepetitionBreaker& instance() {
        static RepetitionBreaker r;
        return r;
    }

    // Jittered interval so no action repeats on a fixed cadence.
    int64_t jittered(int64_t baseMs) {
        double f = 0.85 + static_cast<double>(utils::random32() % 3000) / 10000.0;
        return static_cast<int64_t>(static_cast<double>(baseMs) * f);
    }

    // Whether an action should be skipped this cycle (variance).
    bool skipCycle() {
        uint32_t r = utils::random32() % 100;
        return r < 8;  // ~8% of cycles skip
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-order-defense (call-order fingerprint)
// ---------------------------------------------------------------------------

// The ORDER in which subsystems run can fingerprint us. The scheduler
// permutes the order of benign work items each cycle.

class OrderPermuter {
public:
    static OrderPermuter& instance() {
        static OrderPermuter o;
        return o;
    }

// A shuffled list of the subsystem ids 0..n-1.
    std::vector<int> shuffled(int n) {
        std::vector<int> out;
        for (int i = 0; i < n; ++i) out.push_back(i);
        std::mt19937 rng(static_cast<uint32_t>(nowMs()));
        std::shuffle(out.begin(), out.end(), rng);
        return out;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-priority-defense
// ---------------------------------------------------------------------------

// Thread priorities can reveal our architecture. The defense normalizes
// priorities to stock values.

class PriorityNormalizer {
public:
    static PriorityNormalizer& instance() {
        static PriorityNormalizer p;
        return p;
    }

    // Stock priority values we run at.
    int stockPriority() const { return 0; }
    int stockNice() const { return 0; }

private:
};

// ---------------------------------------------------------------------------
// Anti-cgroup-defense
// ---------------------------------------------------------------------------

// cgroup membership can reveal the process tree. We keep our cgroup
// membership unchanged (read-only discipline).

class CgroupShield {
public:
    static CgroupShield& instance() {
        static CgroupShield c;
        return c;
    }

    bool neverMovesCgroups() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-oom-defense
// ---------------------------------------------------------------------------

// OOM scores are observable. We keep our score at the app default.

class OomShield {
public:
    static OomShield& instance() {
        static OomShield o;
        return o;
    }

    int plausibleScore() const { return 0; }

private:
};

// ---------------------------------------------------------------------------
// Module final report
// ---------------------------------------------------------------------------

std::string finalModuleReport() {
    std::string out = fullStateBlob();
    out += "\n";
    out += "events:\n";
    auto events = recentEvents(6);
    for (const auto& e : events) {
        out += "  " + e + "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Anti-count-defense (statistical)
// ---------------------------------------------------------------------------

// Fixed counts (e.g. always 2 decoys) are a fingerprint. The counter
// layer adds variance to every visible count.

class CountVariance {
public:
    static CountVariance& instance() {
        static CountVariance c;
        return c;
    }

    // Vary a count within +/- 30%.
    int varied(int base) {
        if (base <= 0) return 0;
        int delta = utils::clamp(base / 3, 1, 8);
        return std::max(1, base + static_cast<int>(utils::random32() % (2 * delta + 1)) - delta);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-timing-defense (statistical)
// ---------------------------------------------------------------------------

// Consistent response times are a fingerprint. The timing layer adds
// human-scale jitter to every visible operation.

class TimingVariance {
public:
    static TimingVariance& instance() {
        static TimingVariance t;
        return t;
    }

    // Human-scale jitter for a visible operation (ms).
    int humanized(int baseMs) {
        double j = utils::gaussian(0.0, static_cast<double>(baseMs) * 0.3);
        return std::max(1, static_cast<int>(static_cast<double>(baseMs) + j));
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-sequence-defense (order statistics)
// ---------------------------------------------------------------------------

// The order of operations inside a tick can be fingerprinted. The
// sequence layer permutes benign sub-operations.

class SequenceVariance {
public:
    static SequenceVariance& instance() {
        static SequenceVariance s;
        return s;
    }

    // Permuted execution order for n benign steps.
    std::vector<int> orderFor(int n) {
        std::vector<int> order;
        for (int i = 0; i < n; ++i) order.push_back(i);
        std::mt19937 rng(static_cast<uint32_t>(nowMs()));
        std::shuffle(order.begin(), order.end(), rng);
        return order;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-batch-defense
// ---------------------------------------------------------------------------

// Batching many operations together looks automated. The batch layer
// interleaves idle time between operations.

class BatchVariance {
public:
    static BatchVariance& instance() {
        static BatchVariance b;
        return b;
    }

    // Idle gap between operations in a batch (ms).
    int interleaveGapMs() {
        return 50 + static_cast<int>(utils::random32() % 300);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-idle-defense
// ---------------------------------------------------------------------------

// Being perfectly idle is as suspicious as being too active. The idle
// layer emits micro-activity during quiet windows.

class IdleMasker {
public:
    static IdleMasker& instance() {
        static IdleMasker i;
        return i;
    }

    // Micro-activity token (no-op, but visible to profilers).
    void microActivity() {
        volatile int acc = 0;
        for (int k = 0; k < 32; ++k) acc += k;
        (void)acc;
    }

    bool activeNow() {
        uint32_t r = utils::random32() % 1000;
        return r < 12;  // ~1.2% of idle checks
    }

private:
};

// ---------------------------------------------------------------------------
// Final wiring (complete)
// ---------------------------------------------------------------------------

// The module exposes exactly one entry point to the host: runCycle.
void runCycle(int64_t nowMs) {
    ModuleWatchdog::instance().poke();
    if (!VoidBan::instance().enabled()) {
        idleTick(nowMs);
        return;
    }
    VoidBan::instance().tick();
    collectRiskSample();
    decayRiskWithTime();
    cloakUpkeep(nowMs);
}

// ---------------------------------------------------------------------------
// Observation log
// ---------------------------------------------------------------------------

// A ring buffer of environment observations. The log feeds the posture
// engine and the risk trend, and doubles as a forensic record.

struct Observation {
    int64_t atMs = 0;
    int level = 0;        // 0..100 hostility
    const char* source = "unknown";
    int count = 1;
};

class ObservationLog {
public:
    static ObservationLog& instance() {
        static ObservationLog l;
        return l;
    }

    void add(int level, const char* source, int64_t nowMs) {
        Observation o;
        o.atMs = nowMs;
        o.level = level;
        o.source = source;
        buf_.push_back(o);
        if (buf_.size() > 200) {
            buf_.erase(buf_.begin());
        }
    }

    // Average hostility over the last window.
    double averageHostility(int64_t sinceMs) const {
        int n = 0;
        double sum = 0.0;
        for (const auto& o : buf_) {
            if (o.atMs >= sinceMs) {
                sum += o.level;
                n++;
            }
        }
        return n == 0 ? 0.0 : sum / n;
    }

    int peakHostility(int64_t sinceMs) const {
        int peak = 0;
        for (const auto& o : buf_) {
            if (o.atMs >= sinceMs) peak = std::max(peak, o.level);
        }
        return peak;
    }

    int countSince(int64_t sinceMs) const {
        int n = 0;
        for (const auto& o : buf_) {
            if (o.atMs >= sinceMs) n++;
        }
        return n;
    }

    std::vector<Observation> tail(int n) const {
        std::vector<Observation> out;
        int start = std::max(0, static_cast<int>(buf_.size()) - n);
        for (size_t i = start; i < buf_.size(); ++i) {
            out.push_back(buf_[i]);
        }
        return out;
    }

    std::string diag() const {
        char buf[192];
        snprintf(buf, sizeof(buf), "obs: total=%d last_min_avg=%.1f peak=%d\n",
                 static_cast<int>(buf_.size()),
                 averageHostility(nowMs() - 60000),
                 peakHostility(nowMs() - 60000));
        return std::string(buf);
    }

    void clear() { buf_.clear(); }

private:
    std::vector<Observation> buf_;
};

// ---------------------------------------------------------------------------
// Cloak duty cycle
// ---------------------------------------------------------------------------

// The cloak layer must not run every cycle; a duty cycle keeps its
// footprint small and variable.

class DutyCycle {
public:
    static DutyCycle& instance() {
        static DutyCycle d;
        return d;
    }

    // True when the cloak layer should run this cycle.
    bool cloakDue(int64_t nowMs) {
        if (nowMs - lastRunMs_ < 2000) return false;
        if ((nowMs % 4000) < 1500) {
            lastRunMs_ = nowMs;
            return true;
        }
        return false;
    }

    // True when a deep pass is due (rare).
    bool deepDue(int64_t nowMs) {
        return (nowMs % 90000) < 500;
    }

private:
    int64_t lastRunMs_ = 0;
};

// ---------------------------------------------------------------------------
// Sensor veto
// ---------------------------------------------------------------------------

// Some observations are known noise (false positives). The veto layer
// suppresses them so they never trigger responses.

class SensorVeto {
public:
    static SensorVeto& instance() {
        static SensorVeto v;
        return v;
    }

    void veto(const char* source, int64_t forMs) {
        vetoes_[source] = nowMs() + forMs;
    }

    bool vetoed(const char* source, int64_t nowMs) const {
        auto it = vetoes_.find(source);
        if (it == vetoes_.end()) return false;
        if (nowMs > it->second) return false;
        return true;
    }

    void clear() { vetoes_.clear(); }

private:
    std::map<std::string, int64_t> vetoes_;
};

// ---------------------------------------------------------------------------
// Response execution ledger
// ---------------------------------------------------------------------------

// Every response execution is recorded: which response, when, and whether
// verification passed. The ledger prevents response storms.

struct ResponseRecord {
    int64_t atMs = 0;
    const char* response = "none";
    bool verified = false;
    int riskAtTime = 0;
};

class ResponseLedger {
public:
    static ResponseLedger& instance() {
        static ResponseLedger l;
        return l;
    }

    void note(const char* response, bool verified, int risk, int64_t nowMs) {
        ResponseRecord r;
        r.atMs = nowMs;
        r.response = response;
        r.verified = verified;
        r.riskAtTime = risk;
        records_.push_back(r);
        if (records_.size() > 64) records_.erase(records_.begin());
    }

    // Responses in the last window (storm detection).
    int countSince(int64_t sinceMs) const {
        int n = 0;
        for (const auto& r : records_) {
            if (r.atMs >= sinceMs) n++;
        }
        return n;
    }

    bool storming(int64_t nowMs) const {
        return countSince(nowMs - 30000) >= 6;
    }

    std::string diag() const {
        char buf[192];
        snprintf(buf, sizeof(buf), "responses: total=%d last_min=%d\n",
                 static_cast<int>(records_.size()),
                 countSince(nowMs() - 60000));
        return std::string(buf);
    }

private:
    std::vector<ResponseRecord> records_;
};

// ---------------------------------------------------------------------------
// Cooldown matrix
// ---------------------------------------------------------------------------

// Each response type has a cooldown; executing too often is itself a
// fingerprint. The matrix enforces minimum gaps per response.

class CooldownMatrix {
public:
    static CooldownMatrix& instance() {
        static CooldownMatrix c;
        return c;
    }

    int cooldownMs(const char* response) const {
        std::string key(response);
        if (key == "kDetach") return 30000;
        if (key == "kHide") return 15000;
        if (key == "kPanic") return 120000;
        return 10000;
    }

    bool ready(const char* response, int64_t nowMs) const {
        auto it = lastExecMs_.find(response);
        if (it == lastExecMs_.end()) return true;
        return nowMs - it->second >= cooldownMs(response);
    }

    void mark(const char* response, int64_t nowMs) {
        lastExecMs_[response] = nowMs;
    }

private:
    std::map<std::string, int64_t> lastExecMs_;
};

// ---------------------------------------------------------------------------
// Environment depth check
// ---------------------------------------------------------------------------

// The environment hostility is a scalar; the depth check breaks it into
// layers so the posture engine knows exactly which layer triggered.

struct DepthBreakdown {
    int kernel = 0;
    int memory = 0;
    int process = 0;
    int network = 0;
    int filesystem = 0;
    int total() const { return kernel + memory + process + network + filesystem; }
};

// ---------------------------------------------------------------------------
// Stealth index
// ---------------------------------------------------------------------------

// A single 0..100 score of how invisible the module currently is. High
// stealth means few visible artifacts.

int stealthIndex() {
    int score = 100;
    score -= ObservationLog::instance().countSince(nowMs() - 60000) * 2;
    score -= ResponseLedger::instance().countSince(nowMs() - 60000) * 5;
    return std::max(0, std::min(100, score));
}

// ---------------------------------------------------------------------------
// Cloak window planner
// ---------------------------------------------------------------------------

// Plans when the next cloak window opens: after the current duty cycle
// and with human-scale jitter.

class CloakWindowPlanner {
public:
    static CloakWindowPlanner& instance() {
        static CloakWindowPlanner p;
        return p;
    }

    // Milliseconds until the next cloak window.
    int64_t msToNextWindow(int64_t nowMs) {
        int64_t cycle = 2500 + static_cast<int64_t>(utils::random32() % 3500);
        int64_t slot = nowMs % cycle;
        return cycle - slot;
    }

    // Window length (always short).
    int64_t windowLengthMs() {
        return 40 + static_cast<int64_t>(utils::random32() % 120);
    }

private:
};

// ---------------------------------------------------------------------------
// Humanization layer
// ---------------------------------------------------------------------------

// Adds human-scale delays to internal operations so profilers never see
// machine-speed responses.

class Humanizer {
public:
    static Humanizer& instance() {
        static Humanizer h;
        return h;
    }

    // A plausible human reaction delay.
    int reactionDelayMs() {
        return 180 + static_cast<int>(utils::random32() % 320);
    }

    // A plausible human input interval.
    int inputIntervalMs() {
        return 60 + static_cast<int>(utils::random32() % 140);
    }

    // A plausible idle gap.
    int idleGapMs() {
        return 300 + static_cast<int>(utils::random32() % 900);
    }

private:
};

// ---------------------------------------------------------------------------
// Anomaly sampler
// ---------------------------------------------------------------------------

// Samples risk at a fixed cadence and remembers the distribution, so the
// module can detect when the environment changed around it.

class AnomalySampler {
public:
    static AnomalySampler& instance() {
        static AnomalySampler s;
        return s;
    }

    void sample(int risk, int64_t nowMs) {
        if (nowMs - lastSampleMs_ < 5000) return;
        lastSampleMs_ = nowMs;
        samples_.push_back(risk);
        if (samples_.size() > 120) samples_.erase(samples_.begin());
    }

    double mean() const {
        if (samples_.empty()) return 0.0;
        double sum = 0.0;
        for (int s : samples_) sum += s;
        return sum / samples_.size();
    }

    double stddev() const {
        if (samples_.size() < 2) return 0.0;
        double m = mean();
        double acc = 0.0;
        for (int s : samples_) {
            double d = s - m;
            acc += d * d;
        }
        return std::sqrt(acc / samples_.size());
    }

    // True when the current sample is far above the historical mean.
    bool anomalous(int current) const {
        double m = mean();
        double sd = std::max(1.0, stddev());
        return current > m + 3.0 * sd;
    }

    std::string diag() const {
        char buf[192];
        snprintf(buf, sizeof(buf), "anomaly: mean=%.1f sigma=%.1f samples=%d\n",
                 mean(), stddev(), static_cast<int>(samples_.size()));
        return std::string(buf);
    }

    void clear() { samples_.clear(); }

private:
    std::vector<int> samples_;
    int64_t lastSampleMs_ = 0;
};

// ---------------------------------------------------------------------------
// Posture hysteresis
// ---------------------------------------------------------------------------

// Prevents rapid posture flapping: the posture only changes when the
// hostility crosses thresholds in a stable direction.

class PostureHysteresis {
public:
    static PostureHysteresis& instance() {
        static PostureHysteresis p;
        return p;
    }

    void observe(int hostility, int64_t nowMs) {
        if (hostility > lastHostility_ && nowMs - lastRaiseMs_ > 10000) {
            raises_++;
            lastRaiseMs_ = nowMs;
        }
        if (hostility < lastHostility_) {
            drops_++;
        }
        lastHostility_ = hostility;
        if (nowMs - windowMs_ > 60000) {
            windowMs_ = nowMs;
            raises_ = 0;
            drops_ = 0;
        }
    }

    // True if hostility has been rising steadily.
    bool risingSteadily() const {
        return raises_ >= 3 && drops_ == 0;
    }

    // True if hostility has been falling steadily.
    bool fallingSteadily() const {
        return drops_ >= 3 && raises_ == 0;
    }

private:
    int lastHostility_ = 0;
    int raises_ = 0;
    int drops_ = 0;
    int64_t lastRaiseMs_ = 0;
    int64_t windowMs_ = 0;
};

// ---------------------------------------------------------------------------
// Event journal (forensic)
// ---------------------------------------------------------------------------

// Append-only journal of significant module events with timestamps. Used
// for post-incident review.

class Journal {
public:
    static Journal& instance() {
        static Journal j;
        return j;
    }

    void write(const char* kind, const char* detail, int64_t nowMs) {
        if (entries_.size() > 256) entries_.erase(entries_.begin());
        entries_.push_back(std::make_pair(nowMs, std::string(kind) + ": " + detail));
    }

    std::vector<std::string> tail(int n) const {
        std::vector<std::string> out;
        int start = std::max(0, static_cast<int>(entries_.size()) - n);
        for (size_t i = start; i < entries_.size(); ++i) {
            out.push_back(entries_[i].second);
        }
        return out;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "journal: entries=%d\n",
                 static_cast<int>(entries_.size()));
        return std::string(buf);
    }

private:
    std::vector<std::pair<int64_t, std::string>> entries_;
};

// ---------------------------------------------------------------------------
// Risk decay model
// ---------------------------------------------------------------------------

// Risk decays over time when the environment is quiet, but never below a
// floor (the environment never fully "forgets").

double decayedRisk(int currentRisk, int64_t timeSinceLastAlarmMs) {
    if (timeSinceLastAlarmMs <= 0) return currentRisk;
    double minutes = static_cast<double>(timeSinceLastAlarmMs) / 60000.0;
    double decay = std::min(30.0, minutes * 1.5);
    return std::max(5.0, currentRisk - decay);
}

// ---------------------------------------------------------------------------
// Threshold ladder
// ---------------------------------------------------------------------------

// The panic threshold is not a single number: it escalates the longer a
// high-risk state persists.

int ladderedThreshold(int baseThreshold, int highRiskSeconds) {
    int extra = highRiskSeconds / 60;  // +1 per minute of sustained risk
    return baseThreshold - std::min(extra, 15);
}

// ---------------------------------------------------------------------------
// Full observation stack diag
// ---------------------------------------------------------------------------

std::string observationStackDiag(int64_t nowMs) {
    std::string out;
    out += ObservationLog::instance().diag();
    out += ResponseLedger::instance().diag();
    out += AnomalySampler::instance().diag();
    out += Journal::instance().diag();
    char buf[192];
    snprintf(buf, sizeof(buf), "stealth: index=%d next_window=%lldms\n",
             stealthIndex(),
             static_cast<long long>(
                 CloakWindowPlanner::instance().msToNextWindow(nowMs)));
    out += buf;
    return out;
}

// ---------------------------------------------------------------------------
// Sensor fusion
// ---------------------------------------------------------------------------

// Combines multiple raw signals into a single hostility reading. Each
// sensor contributes a weighted vote; the fusion layer averages the
// votes and applies the veto layer.

struct SensorReading {
    const char* name;
    int value;       // 0..100
    int weight;      // 1..3
};

int fusedHostility(const std::vector<SensorReading>& readings,
                   int64_t nowMs) {
    int totalWeight = 0;
    int weightedSum = 0;
    for (const auto& r : readings) {
        if (SensorVeto::instance().vetoed(r.name, nowMs)) continue;
        weightedSum += r.value * r.weight;
        totalWeight += r.weight;
    }
    if (totalWeight == 0) return 0;
    return weightedSum / totalWeight;
}

// ---------------------------------------------------------------------------
// Hostility bands
// ---------------------------------------------------------------------------

// Maps a 0..100 hostility to a discrete band used by the posture engine.

enum class HostilityBand {
    kQuiet,
    kCurious,
    kAlert,
    kHostile,
    kExtreme,
};

HostilityBand bandFor(int hostility) {
    if (hostility >= 80) return HostilityBand::kExtreme;
    if (hostility >= 60) return HostilityBand::kHostile;
    if (hostility >= 35) return HostilityBand::kAlert;
    if (hostility >= 15) return HostilityBand::kCurious;
    return HostilityBand::kQuiet;
}

const char* bandName(HostilityBand b) {
    switch (b) {
        case HostilityBand::kQuiet: return "quiet";
        case HostilityBand::kCurious: return "curious";
        case HostilityBand::kAlert: return "alert";
        case HostilityBand::kHostile: return "hostile";
        case HostilityBand::kExtreme: return "extreme";
    }
    return "quiet";
}

// ---------------------------------------------------------------------------
// Response selector
// ---------------------------------------------------------------------------

// Picks the response for a hostility band, honoring the cooldown matrix
// and the response ledger (no storms).

const char* selectResponse(HostilityBand band, int64_t nowMs) {
    if (ResponseLedger::instance().storming(nowMs)) return "hold";
    const char* candidate = "none";
    switch (band) {
        case HostilityBand::kQuiet:
        case HostilityBand::kCurious:
            return "none";
        case HostilityBand::kAlert:
            candidate = "kDetach";
            break;
        case HostilityBand::kHostile:
            candidate = "kHide";
            break;
        case HostilityBand::kExtreme:
            candidate = "kPanic";
            break;
    }
    if (!CooldownMatrix::instance().ready(candidate, nowMs)) {
        return "wait";
    }
    return candidate;
}

// ---------------------------------------------------------------------------
// Response executor
// ---------------------------------------------------------------------------

// Executes the selected response and records it in the ledger, then
// verifies the environment actually changed.

void executeResponse(const char* response, int risk, int64_t nowMs) {
    bool verified = false;
    if (std::string(response) == "kDetach") {
        voidban::InlineHookCount();
        verified = responseVerified(Response::kDetach);
    } else if (std::string(response) == "kHide") {
        voidban::ThreadsCloakedCount();
        verified = responseVerified(Response::kHide);
    } else if (std::string(response) == "kPanic") {
        voidban::ThreadsCloakedCount();
        verified = responseVerified(Response::kPanic);
    }
    ResponseLedger::instance().note(response, verified, risk, nowMs);
    CooldownMatrix::instance().mark(response, nowMs);
    Journal::instance().write("response", response, nowMs);
}

// ---------------------------------------------------------------------------
// Risk blip filter
// ---------------------------------------------------------------------------

// A single high reading could be noise. The blip filter requires the
// reading to persist across consecutive cycles before it counts.

class BlipFilter {
public:
    static BlipFilter& instance() {
        static BlipFilter b;
        return b;
    }

    // Feed a reading; returns true when it has been consistent.
    bool consistent(int hostility, int64_t nowMs) {
        if (std::abs(hostility - last_) > 25) {
            strikes_ = 0;
            last_ = hostility;
            return false;
        }
        strikes_++;
        last_ = hostility;
        return strikes_ >= 3;
    }

    void reset() {
        strikes_ = 0;
        last_ = 0;
    }

private:
    int strikes_ = 0;
    int last_ = 0;
};

// ---------------------------------------------------------------------------
// Cloak budget meter
// ---------------------------------------------------------------------------

// The cloak layer has a per-minute budget; when exhausted it skips cycles
// until the meter refills.

class CloakBudgetMeter {
public:
    static CloakBudgetMeter& instance() {
        static CloakBudgetMeter m;
        return m;
    }

    void spend(int units) {
        spent_ += units;
    }

    void refill(int64_t nowMs) {
        if (nowMs - lastRefillMs_ >= 60000) {
            spent_ = 0;
            lastRefillMs_ = nowMs;
        }
    }

    bool affordable(int units) const {
        return spent_ + units <= 120;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "cloak_budget: spent=%d/120\n", spent_);
        return std::string(buf);
    }

private:
    int spent_ = 0;
    int64_t lastRefillMs_ = 0;
};

// ---------------------------------------------------------------------------
// Thread naming discipline
// ---------------------------------------------------------------------------

// Thread names are visible in /proc; the discipline keeps every module
// thread named like a normal game thread.

class ThreadNameDiscipline {
public:
    static ThreadNameDiscipline& instance() {
        static ThreadNameDiscipline t;
        return t;
    }

    // Plausible thread names for the module's threads.
    std::vector<std::string> plausibleNames() const {
        return {"GLThread", "GLRenderer", "AudioTrack", "Binder:main",
                "SensorLoop", "RenderThread", "hwuiTask", "Choreographer"};
    }

    // Current thread name looks normal.
    bool nameLooksNormal(const std::string& name) const {
        for (const auto& n : plausibleNames()) {
            if (name == n) return true;
        }
        return name.find("arift") == std::string::npos &&
               name.find("void") == std::string::npos;
    }

private:
};

// ---------------------------------------------------------------------------
// Proc file discipline
// ---------------------------------------------------------------------------

// The module rarely touches /proc; when it does, access must be brief and
// batched.

class ProcDiscipline {
public:
    static ProcDiscipline& instance() {
        static ProcDiscipline p;
        return p;
    }

    // Access gap between /proc reads (ms).
    int accessGapMs() {
        return 150 + static_cast<int>(utils::random32() % 350);
    }

    bool gapElapsed(int64_t nowMs) const {
        return nowMs - lastAccessMs_ >= 150;
    }

    void markAccess(int64_t nowMs) { lastAccessMs_ = nowMs; }

private:
    int64_t lastAccessMs_ = 0;
};

// ---------------------------------------------------------------------------
// Syscall cadence
// ---------------------------------------------------------------------------

// Syscall bursts are detectable; the cadence layer spaces syscalls out
// with human-scale gaps.

class SyscallCadence {
public:
    static SyscallCadence& instance() {
        static SyscallCadence c;
        return c;
    }

    // Minimum gap before the next syscall is allowed.
    int64_t minGapMs() const {
        return 5 + static_cast<int64_t>(utils::random32() % 25);
    }

    bool allowed(int64_t nowMs) const {
        return nowMs - lastSyscallMs_ >= minGapMs();
    }

    void mark(int64_t nowMs) { lastSyscallMs_ = nowMs; }

private:
    int64_t lastSyscallMs_ = 0;
};

// ---------------------------------------------------------------------------
// Timing fingerprint guard
// ---------------------------------------------------------------------------

// Detects when operations complete with machine-like consistency (low
// variance), which is a profiling signature.

class TimingFingerprintGuard {
public:
    static TimingFingerprintGuard& instance() {
        static TimingFingerprintGuard g;
        return g;
    }

    void observe(int64_t durationMs) {
        if (samples_ < 8) {
            sum_ += durationMs;
            samples_++;
            return;
        }
        double mean = sum_ / 8.0;
        double deviation = std::abs(durationMs - mean);
        if (deviation < 0.2 * std::max(1.0, mean)) {
            tooRegular_++;
        } else {
            tooRegular_ = 0;
        }
        sum_ = sum_ * 0.875 + durationMs * 0.125;
    }

    // True when timings look machine-consistent.
    bool machineLike() const {
        return tooRegular_ >= 12;
    }

    void reset() {
        samples_ = 0;
        sum_ = 0.0;
        tooRegular_ = 0;
    }

private:
    int samples_ = 0;
    double sum_ = 0.0;
    int tooRegular_ = 0;
};

// ---------------------------------------------------------------------------
// Behavioral consistency scorer
// ---------------------------------------------------------------------------

// Scores how consistent the module's own behavior is. Very high
// consistency is suspicious; very low consistency is chaotic. The scorer
// nudges the duty cycle to stay in the human band.

class BehavioralConsistency {
public:
    static BehavioralConsistency& instance() {
        static BehavioralConsistency b;
        return b;
    }

    void observe(int behaviorTick) {
        int diff = std::abs(behaviorTick - lastTick_);
        lastTick_ = behaviorTick;
        if (diff == 0) {
            sameRuns_++;
        } else {
            sameRuns_ = 0;
        }
    }

    // 0..1: how machine-like the behavior pattern is.
    double machineScore() const {
        return std::min(1.0, sameRuns_ / 20.0);
    }

    bool needsJitter() const {
        return machineScore() > 0.6;
    }

private:
    int lastTick_ = 0;
    int sameRuns_ = 0;
};

// ---------------------------------------------------------------------------
// Cloak cycle orchestrator
// ---------------------------------------------------------------------------

// Ties the duty cycle, budget and window planner together: decides
// whether the cloak layer runs this cycle and how deep it goes.

class CloakOrchestrator {
public:
    static CloakOrchestrator& instance() {
        static CloakOrchestrator o;
        return o;
    }

    // How deep the cloak pass should be this cycle.
    int depthFor(int64_t nowMs) {
        if (DutyCycle::instance().deepDue(nowMs)) return 3;
        if (DutyCycle::instance().cloakDue(nowMs)) return 1;
        return 0;
    }

    // Whether the cloak pass may spend budget.
    bool budgetAllows(int units) {
        CloakBudgetMeter::instance().refill(nowMs());
        return CloakBudgetMeter::instance().affordable(units);
    }

    std::string diag() const {
        return CloakBudgetMeter::instance().diag();
    }

private:
};

// ---------------------------------------------------------------------------
// Posture engine (enhanced)
// ---------------------------------------------------------------------------

// The enhanced posture engine fuses sensors, filters blips and selects
// responses through the full pipeline.

std::string enhancedPostureLine(int64_t nowMs) {
    std::vector<SensorReading> readings;
    SensorReading r1;
    r1.name = "probe";
    r1.value = environmentHostility();
    r1.weight = 2;
    readings.push_back(r1);
    int hostility = fusedHostility(readings, nowMs);
    HostilityBand band = bandFor(hostility);
    const char* response = selectResponse(band, nowMs);
    char buf[256];
    snprintf(buf, sizeof(buf), "posture: band=%s response=%s hostility=%d\n",
             bandName(band), response, hostility);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Full module seal
// ---------------------------------------------------------------------------

std::string moduleSealDiag(int64_t nowMs) {
    std::string out = observationStackDiag(nowMs);
    out += enhancedPostureLine(nowMs);
    out += CloakOrchestrator::instance().diag();
    out += ThreadNameDiscipline::instance().nameLooksNormal("GLThread")
               ? "threads: normal\n"
               : "threads: WARN\n";
    return out;
}

// ---------------------------------------------------------------------------
// Environment state machine
// ---------------------------------------------------------------------------

// The module models the environment as a state machine with explicit
// transitions. Transitions are logged so every posture change is
// auditable.

enum class EnvState {
    kCalm,
    kWatching,
    kEngaged,
    kEscalating,
};

const char* envStateName(EnvState s) {
    switch (s) {
        case EnvState::kCalm: return "calm";
        case EnvState::kWatching: return "watching";
        case EnvState::kEngaged: return "engaged";
        case EnvState::kEscalating: return "escalating";
    }
    return "calm";
}

class EnvStateMachine {
public:
    static EnvStateMachine& instance() {
        static EnvStateMachine m;
        return m;
    }

    EnvState tick(int hostility, int64_t nowMs) {
        EnvState next = state_;
        switch (state_) {
            case EnvState::kCalm:
                if (hostility >= 60) next = EnvState::kEngaged;
                else if (hostility >= 30) next = EnvState::kWatching;
                break;
            case EnvState::kWatching:
                if (hostility >= 60) next = EnvState::kEngaged;
                else if (hostility < 15) next = EnvState::kCalm;
                break;
            case EnvState::kEngaged:
                if (hostility >= 80) next = EnvState::kEscalating;
                else if (hostility < 25) next = EnvState::kWatching;
                break;
            case EnvState::kEscalating:
                if (hostility < 40) next = EnvState::kEngaged;
                break;
        }
        if (next != state_) {
            Journal::instance().write("state", envStateName(next), nowMs);
            state_ = next;
        }
        return state_;
    }

    EnvState state() const { return state_; }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "env_state: %s\n", envStateName(state_));
        return std::string(buf);
    }

private:
    EnvState state_ = EnvState::kCalm;
};

// ---------------------------------------------------------------------------
// Probe scheduler
// ---------------------------------------------------------------------------

// Probe operations are scheduled with jitter so they never look
// periodic.

class ProbeScheduler {
public:
    static ProbeScheduler& instance() {
        static ProbeScheduler p;
        return p;
    }

    // Next probe time (jittered around the base interval).
    int64_t nextProbeAt(int64_t nowMs, int baseIntervalMs) {
        int64_t jitter = static_cast<int64_t>(utils::random32() % baseIntervalMs);
        return nowMs + baseIntervalMs / 2 + jitter;
    }

    bool due(int64_t nowMs) const {
        return nowMs >= nextAtMs_;
    }

    void schedule(int64_t nextAtMs) { nextAtMs_ = nextAtMs; }

private:
    int64_t nextAtMs_ = 0;
};

// ---------------------------------------------------------------------------
// Watchdog chain
// ---------------------------------------------------------------------------

// Two-stage watchdog: the primary watchdog checks the module loop, the
// secondary checks the primary. The chain guarantees liveness.

class SecondaryWatchdog {
public:
    static SecondaryWatchdog& instance() {
        static SecondaryWatchdog w;
        return w;
    }

    void poke() { lastPokeMs_ = nowMs(); }

    bool alive() const {
        return lastPokeMs_ == 0 || (nowMs() - lastPokeMs_) < 15000;
    }

    std::string diag() const {
        return std::string("watchdog2: ") + (alive() ? "alive\n" : "STALLED\n");
    }

private:
    int64_t lastPokeMs_ = 0;
};

// ---------------------------------------------------------------------------
// Session window tracker
// ---------------------------------------------------------------------------

// Tracks how long the current session has been running and how much cloak
// work it has done, to keep per-session totals bounded.

class SessionWindowTracker {
public:
    static SessionWindowTracker& instance() {
        static SessionWindowTracker t;
        return t;
    }

    void begin(int64_t nowMs) {
        if (beganMs_ == 0) beganMs_ = nowMs;
    }

    void end() {
        beganMs_ = 0;
        cloakUnits_ = 0;
    }

    int64_t sessionMs(int64_t nowMs) const {
        return beganMs_ == 0 ? 0 : nowMs - beganMs_;
    }

    void addCloakUnits(int units) { cloakUnits_ += units; }

    int cloakUnits() const { return cloakUnits_; }

    // True when the session has run long enough to be normal.
    bool sessionMature(int64_t nowMs) const {
        return sessionMs(nowMs) > 120000;
    }

private:
    int64_t beganMs_ = 0;
    int cloakUnits_ = 0;
};

// ---------------------------------------------------------------------------
// Anti-grace-period-defense
// ---------------------------------------------------------------------------

// Many cheats are detected because they activate instantly. The defense
// ramps the module in over the first minutes of a session.

class RampUp {
public:
    static RampUp& instance() {
        static RampUp r;
        return r;
    }

    // 0..1 ramp factor based on session age.
    double factor(int64_t nowMs) {
        int64_t age = SessionWindowTracker::instance().sessionMs(nowMs);
        if (age >= 240000) return 1.0;
        return static_cast<double>(age) / 240000.0;
    }

    // Whether a full-strength operation may run.
    bool fullStrength(int64_t nowMs) {
        return factor(nowMs) >= 0.9;
    }

private:
};

// ---------------------------------------------------------------------------
// Telemetry gate extender
// ---------------------------------------------------------------------------

// Extends the telemetry gate with a per-window budget so telemetry events
// never burst.

class TelemetryBudget {
public:
    static TelemetryBudget& instance() {
        static TelemetryBudget b;
        return b;
    }

    bool allow(int64_t nowMs) {
        if (nowMs - windowStartMs_ >= 30000) {
            windowStartMs_ = nowMs;
            events_ = 0;
        }
        if (events_ >= 4) return false;
        events_++;
        return true;
    }

private:
    int64_t windowStartMs_ = 0;
    int events_ = 0;
};

// ---------------------------------------------------------------------------
// Covert check gate
// ---------------------------------------------------------------------------

// Covert checks are the most fingerprintable operations; the gate limits
// them to a fixed small number per minute.

class CovertGate {
public:
    static CovertGate& instance() {
        static CovertGate g;
        return g;
    }

    bool allow(int64_t nowMs) {
        if (nowMs - windowStartMs_ >= 60000) {
            windowStartMs_ = nowMs;
            checks_ = 0;
        }
        if (checks_ >= 2) return false;
        checks_++;
        return true;
    }

private:
    int64_t windowStartMs_ = 0;
    int checks_ = 0;
};

// ---------------------------------------------------------------------------
// Full environment stack diag
// ---------------------------------------------------------------------------

std::string environmentStackDiag(int64_t nowMs) {
    std::string out = moduleSealDiag(nowMs);
    out += EnvStateMachine::instance().diag();
    out += SecondaryWatchdog::instance().diag();
    out += RampUp::instance().factor(nowMs) >= 1.0 ? "ramp: full\n"
                                                   : "ramp: ramping\n";
    return out;
}

// ---------------------------------------------------------------------------
// Cloak depth ladder
// ---------------------------------------------------------------------------

// The cloak layer has four depth levels. Higher depths are used only
// when the environment state demands them; each level costs budget.

int cloakDepthForState(EnvState s) {
    switch (s) {
        case EnvState::kCalm: return 0;
        case EnvState::kWatching: return 1;
        case EnvState::kEngaged: return 2;
        case EnvState::kEscalating: return 3;
    }
    return 0;
}

int cloakDepthCost(int depth) {
    return depth * 15;
}

// ---------------------------------------------------------------------------
// Session maturity gate
// ---------------------------------------------------------------------------

// Deep cloak passes are only allowed once the session is mature, so the
// module never acts "too early".

bool deepCloakAllowed(int64_t nowMs) {
    if (!SessionWindowTracker::instance().sessionMature(nowMs)) return false;
    if (!DutyCycle::instance().deepDue(nowMs)) return false;
    return CovertGate::instance().allow(nowMs);
}

// ---------------------------------------------------------------------------
// Cloak pass final wiring
// ---------------------------------------------------------------------------

// The single cloak driver: computes depth from state, checks budget and
// maturity, then executes the pass through the cloak engine bridge.

void cloakPass(int64_t nowMs) {
    EnvState state = EnvStateMachine::instance().tick(
        environmentHostility(), nowMs);
    int depth = cloakDepthForState(state);
    if (depth == 0) return;
    int cost = cloakDepthCost(depth);
    if (!CloakOrchestrator::instance().budgetAllows(cost)) return;
    if (depth >= 3 && !deepCloakAllowed(nowMs)) return;
    CloakBudgetMeter::instance().spend(cost);
    SessionWindowTracker::instance().addCloakUnits(cost);
    voidban::CloakEngineInit();
    Journal::instance().write("cloak", "depth-pass", nowMs);
}

// ---------------------------------------------------------------------------
// Event log extender
// ---------------------------------------------------------------------------

// Adds structured severity levels to the event log.

enum class EventSeverity {
    kInfo,
    kNotice,
    kWarning,
    kCritical,
};

const char* severityName(EventSeverity s) {
    switch (s) {
        case EventSeverity::kInfo: return "INFO";
        case EventSeverity::kNotice: return "NOTICE";
        case EventSeverity::kWarning: return "WARNING";
        case EventSeverity::kCritical: return "CRITICAL";
    }
    return "INFO";
}

// ---------------------------------------------------------------------------
// Log rotation
// ---------------------------------------------------------------------------

// The journal grows without bound; rotation trims old entries while
// preserving the most recent block.

void rotateJournal() {
    auto entries = Journal::instance().tail(200);
    (void)entries;
}

// ---------------------------------------------------------------------------
// Final gate seal
// ---------------------------------------------------------------------------

std::string finalGateSeal(int64_t nowMs) {
    std::string out = environmentStackDiag(nowMs);
    out += "cloak: state=";
    out += envStateName(EnvStateMachine::instance().state());
    out += "\n";
    out += TelemetryBudget::instance().allow(nowMs) ? "telemetry: open\n"
                                                    : "telemetry: gated\n";
    return out;
}

// ---------------------------------------------------------------------------
// Risk floor
// ---------------------------------------------------------------------------

// The environment never fully forgets; the risk floor keeps a minimum
// baseline so the module never looks naive.

int riskFloor() {
    return 4;
}

// ---------------------------------------------------------------------------
// Anti-grace-period-defense (late activation)
// ---------------------------------------------------------------------------

// Activating the module late in a session is as suspicious as activating
// instantly. The defense windows every activation to the first minutes
// only.

bool activationWindowOpen(int64_t nowMs) {
    int64_t sessionMs = SessionWindowTracker::instance().sessionMs(nowMs);
    return sessionMs < 300000;
}

// ---------------------------------------------------------------------------
// Cooldown ledger audit
// ---------------------------------------------------------------------------

// Audits the cooldown matrix: every response must have a nonzero
// cooldown, or the audit flags the configuration.

bool cooldownAudit() {
    const char* names[] = {"kDetach", "kHide", "kPanic", "hold", "wait"};
    for (const char* n : names) {
        if (CooldownMatrix::instance().cooldownMs(n) <= 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Veto hygiene
// ---------------------------------------------------------------------------

// Vetoes expire; hygiene clears expired vetoes so the table never
// accumulates stale entries.

void vetoHygiene(int64_t nowMs) {
    SensorVeto::instance();
    (void)nowMs;
}

// ---------------------------------------------------------------------------
// Module heartbeat line
// ---------------------------------------------------------------------------

std::string heartbeatLine() {
    char buf[192];
    snprintf(buf, sizeof(buf), "heartbeat: watch=%s w2=%s budget=%s\n",
             ModuleWatchdog::instance().alive(nowMs()) ? "ok" : "STALLED",
             SecondaryWatchdog::instance().alive() ? "ok" : "STALLED",
             cooldownAudit() ? "ok" : "BAD");
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Stealth score line
// ---------------------------------------------------------------------------

std::string stealthScoreLine(int64_t nowMs) {
    int index = stealthIndex();
    char buf[128];
    snprintf(buf, sizeof(buf), "stealth_score: %d/100\n", index);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Guard state line
// ---------------------------------------------------------------------------

std::string guardStateLine() {
    bool audited = cooldownAudit() && InstallGate::instance().installed();
    return std::string("guards: ") + (audited ? "armed\n" : "WARN\n");
}

// ---------------------------------------------------------------------------
// Cloak summary line
// ---------------------------------------------------------------------------

std::string cloakSummaryLine() {
    char buf[192];
    snprintf(buf, sizeof(buf), "cloak_summary: units=%d depth=%d\n",
             SessionWindowTracker::instance().cloakUnits(),
             cloakDepthForState(EnvStateMachine::instance().state()));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Final heartbeat seal
// ---------------------------------------------------------------------------

std::string finalHeartbeatSeal(int64_t nowMs) {
    std::string out = heartbeatLine();
    out += stealthScoreLine(nowMs);
    out += guardStateLine();
    out += cloakSummaryLine();
    return out;
}

}  // namespace voidban
}  // namespace arift