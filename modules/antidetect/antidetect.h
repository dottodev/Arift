#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace arift {
namespace antidetect {

// Overall stealth posture the anti-detection layer enforces.
enum class Posture {
    kNormal = 0,      // nothing suspicious observed
    kWatch = 1,       // probes observed, hold steady
    kGuard = 2,       // moderate pressure, reduce activity
    kShield = 3,      // high pressure, cloak aggressively
    kRetreat = 4,     // critical, withdraw
};

// A single observation of a potential detection vector.
struct Observation {
    int64_t atMs = 0;
    int severity = 0;          // 0..100
    const char* source = "unknown";
    int count = 1;
};

// Environment fingerprint snapshot.
struct Fingerprint {
    int pid = 0;
    int threadCount = 0;
    int mapCount = 0;
    int executableRegions = 0;
    int writableExecutableRegions = 0;
    int anonymousRx = 0;
    uint64_t totalMapsBytes = 0;
    bool tracerAttached = false;
    bool debuggerAttached = false;
    bool seLinuxEnforcing = false;
    int fdCount = 0;
    std::string processName;
    std::string packageName;
};

// Self-audit result.
struct AuditResult {
    bool passed = true;
    int totalChecks = 0;
    int failedChecks = 0;
    std::vector<std::string> failures;
    int64_t auditedAtMs = 0;
};

// Stealth metrics.
struct StealthMetrics {
    int stealthIndex = 100;     // 0..100
    int riskScore = 0;          // 0..100
    int probesSeen = 0;
    int anomalies = 0;
    int64_t lastProbeMs = 0;
    int64_t lastEscalationMs = 0;
    Posture posture = Posture::kNormal;
};

// Runtime config.
struct AntiDetectConfig {
    bool enabled = true;
    int auditIntervalMs = 5000;
    int probeWindowMs = 30000;
    int watchThreshold = 25;    // severity to enter kWatch
    int guardThreshold = 50;
    int shieldThreshold = 75;
    int retreatThreshold = 90;
    int stealthFloor = 40;
    bool autoAudit = true;
    bool autoCloak = true;
    bool logEvents = true;
    int mapNormalMax = 60;      // expected map region count
    int threadNormalMax = 60;
};

// Response action chosen by the layer.
enum class Action {
    kNone = 0,
    kSteady = 1,     // hold current behavior
    kReduce = 2,     // reduce visible activity
    kCloak = 3,      // enter cloak posture
    kHibernate = 4,  // pause module work
    kPanic = 5,      // emergency shutdown of risky ops
};

// Anti-detection coordinator: the single file every module asks for
// clearance before doing something visible.
class AntiDetect {
public:
    static AntiDetect& instance();

    void configure(const AntiDetectConfig& cfg);
    const AntiDetectConfig& config() const;

    void tick(int64_t nowMs);

    // Ask whether a visible operation is allowed right now.
    bool clearance(const char* op, int weight, int64_t nowMs);

    // Record that a probe/scan was detected.
    void noteProbe(const char* source, int severity, int64_t nowMs);

    // Record a suspicious system call / access.
    void noteAnomaly(const char* source, int64_t nowMs);

    // Run a full self-audit of the process surface.
    AuditResult audit(int64_t nowMs);

    // Get the current fingerprint.
    Fingerprint fingerprint() const;

    // Metrics snapshot for HUD/diagnostics.
    StealthMetrics metrics() const;

    std::string diag() const;

    void setEnabled(bool on);
    bool enabled() const;

private:
    AntiDetect() = default;
    ~AntiDetect() = default;

    Posture computePosture(int risk) const;
    void escalate(const char* source, int risk, int64_t nowMs);
    void rotateWindow(int64_t nowMs);

    mutable std::mutex mutex_;
    AntiDetectConfig cfg_;
    std::atomic<bool> enabled_{true};
    std::atomic<int> risk_{0};
    std::atomic<int> stealth_{100};
    std::atomic<Posture> posture_{Posture::kNormal};
    std::atomic<int64_t> lastProbeMs_{0};
    std::atomic<int64_t> lastEscalationMs_{0};
    std::atomic<int> probesInWindow_{0};
    std::atomic<int> anomalies_{0};
    std::atomic<int64_t> windowStartMs_{0};
    std::vector<Observation> observations_;
};

// Free-function API used by other modules (thin wrappers).
void initialize();
void shutdown();
bool safeToAct(const char* op, int weight);
void detected(const char* source, int severity);
void setEnabled(bool on);
bool isEnabled();
std::string statusLine();
std::string fullReport();

}  // namespace antidetect
}  // namespace arift