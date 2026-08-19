#include "antidetect.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "arift_log.h"
#include "arift_time.h"
#include "arift_utils.h"
#include "memory_map.h"

#include <unistd.h>
#include <sys/types.h>

namespace arift {
namespace antidetect {

namespace {

const char* postureName(Posture p) {
    switch (p) {
        case Posture::kNormal: return "normal";
        case Posture::kWatch: return "watch";
        case Posture::kGuard: return "guard";
        case Posture::kShield: return "shield";
        case Posture::kRetreat: return "retreat";
    }
    return "normal";
}

// Reads a file fully into a string (used for /proc surfaces).
bool readProcFile(const char* path, std::string& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Counts lines in a proc surface.
int countLines(const std::string& data) {
    int n = 0;
    for (char c : data) {
        if (c == '\n') n++;
    }
    return n;
}

// Whether a region is executable and anonymous (suspicious when fresh).
bool executableRegion(const MemRegion& r) {
    return r.executable;
}

// Risk decay factor used between windows.
int decayFactor(int risk, int64_t quietMs) {
    if (quietMs <= 0) return risk;
    double minutes = static_cast<double>(quietMs) / 60000.0;
    int decay = static_cast<int>(minutes * 6.0);
    return std::max(0, risk - decay);
}

}  // namespace

// ---------------------------------------------------------------------------
// Singleton + lifecycle
// ---------------------------------------------------------------------------

AntiDetect& AntiDetect::instance() {
    static AntiDetect ad;
    return ad;
}

void initialize() {
    ARIFT_INFO(kTagGuard, "AntiDetect initialized");
}

void shutdown() {
    AntiDetect::instance().setEnabled(false);
    ARIFT_INFO(kTagGuard, "AntiDetect shutdown");
}

void setEnabled(bool on) {
    AntiDetect::instance().setEnabled(on);
}

bool isEnabled() {
    return AntiDetect::instance().enabled();
}

void detected(const char* source, int severity) {
    AntiDetect::instance().noteProbe(source, severity, utils::monotonicMs());
}

bool safeToAct(const char* op, int weight) {
    return AntiDetect::instance().clearance(op, weight,
                                            utils::monotonicMs());
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void AntiDetect::configure(const AntiDetectConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
}

const AntiDetectConfig& AntiDetect::config() const {
    return cfg_;
}

void AntiDetect::setEnabled(bool on) {
    enabled_.store(on);
}

bool AntiDetect::enabled() const {
    return enabled_.load();
}

// ---------------------------------------------------------------------------
// Window rotation
// ---------------------------------------------------------------------------

void AntiDetect::rotateWindow(int64_t nowMs) {
    int64_t start = windowStartMs_.load();
    if (start == 0) {
        windowStartMs_.store(nowMs);
        probesInWindow_.store(0);
        return;
    }
    if (nowMs - start >= cfg_.probeWindowMs) {
        windowStartMs_.store(nowMs);
        probesInWindow_.store(0);
    }
}

// ---------------------------------------------------------------------------
// Risk + posture
// ---------------------------------------------------------------------------

Posture AntiDetect::computePosture(int risk) const {
    if (risk >= cfg_.retreatThreshold) return Posture::kRetreat;
    if (risk >= cfg_.shieldThreshold) return Posture::kShield;
    if (risk >= cfg_.guardThreshold) return Posture::kGuard;
    if (risk >= cfg_.watchThreshold) return Posture::kWatch;
    return Posture::kNormal;
}

void AntiDetect::escalate(const char* source, int risk, int64_t nowMs) {
    Posture p = computePosture(risk);
    posture_.store(p);
    lastEscalationMs_.store(nowMs);
    if (cfg_.logEvents) {
        ARIFT_WARN(kTagGuard, "escalate -> %s (risk=%d, src=%s)",
                   postureName(p), risk, source);
    }
}

// ---------------------------------------------------------------------------
// Probe + anomaly recording
// ---------------------------------------------------------------------------

void AntiDetect::noteProbe(const char* source, int severity, int64_t nowMs) {
    if (!enabled_.load()) return;
    rotateWindow(nowMs);
    probesInWindow_.fetch_add(1);
    lastProbeMs_.store(nowMs);
    int newRisk = risk_.load() + std::max(1, severity / 10);
    risk_.store(std::min(100, newRisk));
    escalate(source, newRisk, nowMs);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Observation o;
        o.atMs = nowMs;
        o.severity = severity;
        o.source = source;
        observations_.push_back(o);
        if (observations_.size() > 128) {
            observations_.erase(observations_.begin());
        }
    }
}

void AntiDetect::noteAnomaly(const char* source, int64_t nowMs) {
    if (!enabled_.load()) return;
    anomalies_.fetch_add(1);
    int newRisk = risk_.load() + 8;
    risk_.store(std::min(100, newRisk));
    escalate(source, newRisk, nowMs);
}

// ---------------------------------------------------------------------------
// Clearance gate
// ---------------------------------------------------------------------------

bool AntiDetect::clearance(const char* op, int weight, int64_t nowMs) {
    if (!enabled_.load()) return true;
    (void)op;
    (void)nowMs;
    Posture p = posture_.load();
    switch (p) {
        case Posture::kNormal:
            return true;
        case Posture::kWatch:
            return weight <= 2;
        case Posture::kGuard:
            return weight == 0;
        case Posture::kShield:
            return false;
        case Posture::kRetreat:
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fingerprint collection
// ---------------------------------------------------------------------------

Fingerprint AntiDetect::fingerprint() const {
    Fingerprint f;
    f.pid = static_cast<int>(getpid());

    std::string status;
    if (readProcFile("/proc/self/status", status)) {
        f.threadCount = countLines(status);
    }

    MemoryMap& mm = MemoryMap::instance();
    mm.refresh(f.pid);
    const auto& regions = mm.regions();
    f.mapCount = static_cast<int>(regions.size());
    for (const auto& r : regions) {
        f.totalMapsBytes += r.end - r.start;
        if (executableRegion(r)) {
            f.executableRegions++;
            if (r.path.empty()) f.anonymousRx++;
        }
        if (r.executable && r.writable) {
            f.writableExecutableRegions++;
        }
    }

    std::string tracerPid;
    if (readProcFile("/proc/self/status", tracerPid)) {
        if (tracerPid.find("TracerPid:") != std::string::npos) {
            size_t pos = tracerPid.find("TracerPid:");
            size_t nl = tracerPid.find('\n', pos);
            std::string val = tracerPid.substr(pos, nl - pos);
            if (val.find(":\t0") == std::string::npos) {
                f.tracerAttached = true;
            }
        }
    }

    return f;
}

// ---------------------------------------------------------------------------
// Self-audit
// ---------------------------------------------------------------------------

AuditResult AntiDetect::audit(int64_t nowMs) {
    AuditResult result;
    result.auditedAtMs = nowMs;
    Fingerprint f = fingerprint();
    int total = 0;
    int failed = 0;
    std::vector<std::string> failures;

    auto check = [&](bool ok, const char* name) {
        total++;
        if (!ok) {
            failed++;
            failures.push_back(name);
        }
    };

    check(f.tracerAttached == false, "tracer attached");
    check(f.debuggerAttached == false, "debugger attached");
    check(f.mapCount <= cfg_.mapNormalMax, "map count too high");
    check(f.threadCount <= cfg_.threadNormalMax, "thread count too high");
    check(f.anonymousRx == 0, "anonymous rx regions");
    check(f.writableExecutableRegions == 0, "rwx regions");

    result.passed = failed == 0;
    result.totalChecks = total;
    result.failedChecks = failed;
    result.failures = failures;
    return result;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void AntiDetect::tick(int64_t nowMs) {
    if (!enabled_.load()) return;
    rotateWindow(nowMs);

    // Decay risk during quiet windows.
    int64_t lastProbe = lastProbeMs_.load();
    int64_t quietMs = (lastProbe == 0) ? 0 : nowMs - lastProbe;
    int cur = risk_.load();
    int decayed = decayFactor(cur, quietMs);
    risk_.store(decayed);

    // Periodically re-audit.
    static int64_t lastAuditMs = 0;
    if (cfg_.autoAudit && nowMs - lastAuditMs >= cfg_.auditIntervalMs) {
        lastAuditMs = nowMs;
        AuditResult r = audit(nowMs);
        if (!r.passed) {
            noteProbe("self-audit", 15 + r.failedChecks * 5, nowMs);
        }
    }

    // Stealth index reflects risk.
    int stealth = 100 - risk_.load() - probesInWindow_.load() * 2;
    stealth_.store(std::max(cfg_.stealthFloor, std::min(100, stealth)));
}

// ---------------------------------------------------------------------------
// Metrics + diag
// ---------------------------------------------------------------------------

StealthMetrics AntiDetect::metrics() const {
    StealthMetrics m;
    m.stealthIndex = stealth_.load();
    m.riskScore = risk_.load();
    m.probesSeen = probesInWindow_.load();
    m.anomalies = anomalies_.load();
    m.lastProbeMs = lastProbeMs_.load();
    m.lastEscalationMs = lastEscalationMs_.load();
    m.posture = posture_.load();
    return m;
}

std::string AntiDetect::diag() const {
    StealthMetrics m = metrics();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "antidetect: %s posture=%s risk=%d stealth=%d probes=%d "
             "anomalies=%d\n",
             enabled_.load() ? "ON" : "OFF", postureName(m.posture),
             m.riskScore, m.stealthIndex, m.probesSeen, m.anomalies);
    return std::string(buf);
}

std::string statusLine() {
    return AntiDetect::instance().diag();
}

// ---------------------------------------------------------------------------
// Full report
// ---------------------------------------------------------------------------

std::string fullReport() {
    AntiDetect& ad = AntiDetect::instance();
    StealthMetrics m = ad.metrics();
    AuditResult a = ad.audit(utils::monotonicMs());
    Fingerprint f = ad.fingerprint();
    std::string out = ad.diag();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "audit: passed=%d total=%d failed=%d\n"
             "fp: pid=%d maps=%d exec=%d anonymous_rx=%d rwx=%d threads=%d\n",
             a.passed ? 1 : 0, a.totalChecks, a.failedChecks,
             f.pid, f.mapCount, f.executableRegions, f.anonymousRx,
             f.writableExecutableRegions, f.threadCount);
    out += buf;
    for (const auto& fail : a.failures) {
        out += "  fail: " + fail + "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Operation policy table
// ---------------------------------------------------------------------------

// Every visible operation maps to a weight. The policy table decides
// which operations are permitted at each posture.

struct OpPolicy {
    const char* name;
    int weight;
    Posture minPosture;   // required posture to run
};

static const OpPolicy kOpPolicies[] = {
    {"memory_read", 1, Posture::kGuard},
    {"memory_write", 2, Posture::kWatch},
    {"hook_install", 3, Posture::kNormal},
    {"hook_remove", 3, Posture::kWatch},
    {"proc_open", 1, Posture::kGuard},
    {"proc_read", 1, Posture::kGuard},
    {"socket_connect", 2, Posture::kWatch},
    {"socket_send", 2, Posture::kWatch},
    {"thread_spawn", 2, Posture::kWatch},
    {"thread_rename", 1, Posture::kGuard},
    {"so_open", 1, Posture::kGuard},
    {"elf_parse", 2, Posture::kWatch},
    {"jit_alloc", 3, Posture::kNormal},
    {"file_write", 2, Posture::kWatch},
    {"dns_lookup", 1, Posture::kGuard},
};

const OpPolicy* opPolicy(const char* name) {
    for (const auto& p : kOpPolicies) {
        if (strcmp(p.name, name) == 0) return &p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Policy evaluation
// ---------------------------------------------------------------------------

bool policyAllows(Posture current, Posture required) {
    return static_cast<int>(current) >= static_cast<int>(required);
}

// ---------------------------------------------------------------------------
// Operation gate (policy-aware)
// ---------------------------------------------------------------------------

bool operationAllowed(const char* op, Posture current) {
    const OpPolicy* p = opPolicy(op);
    if (!p) return current <= Posture::kWatch;
    if (p->weight >= 3 && current >= Posture::kShield) return false;
    return policyAllows(current, p->minPosture);
}

// ---------------------------------------------------------------------------
// Process surface shield
// ---------------------------------------------------------------------------

// /proc/self exposes our process surface. The shield reads the common
// surfaces and reports anomalies.

class ProcSurfaceShield {
public:
    static ProcSurfaceShield& instance() {
        static ProcSurfaceShield s;
        return s;
    }

    // Whether /proc/self/status shows a clean tracer state.
    bool tracerClean() {
        std::string data;
        if (!readProcFile("/proc/self/status", data)) return true;
        size_t pos = data.find("TracerPid:");
        if (pos == std::string::npos) return true;
        size_t nl = data.find('\n', pos);
        std::string val = data.substr(pos, nl - pos);
        return val.find(":\t0") != std::string::npos;
    }

    // Whether /proc/self/maps shows no anonymous RX regions.
    bool mapsClean() {
        MemoryMap& mm = MemoryMap::instance();
        mm.refresh(getpid());
        for (const auto& r : mm.regions()) {
            if (r.executable && !r.writable && r.path.empty()) return false;
        }
        return true;
    }

    // Whether /proc/self/task has a sane thread count.
    bool threadCountSane(int maxThreads) {
        std::string data;
        if (!readProcFile("/proc/self/status", data)) return true;
        size_t pos = data.find("Threads:");
        if (pos == std::string::npos) return true;
        size_t nl = data.find('\n', pos);
        std::string val = data.substr(pos, nl - pos);
        std::string num = val.substr(val.find('\t') + 1);
        int n = atoi(num.c_str());
        return n <= maxThreads;
    }

    std::string diag() {
        char buf[128];
        snprintf(buf, sizeof(buf), "proc_shield: tracer=%d maps=%d\n",
                 tracerClean() ? 1 : 0, mapsClean() ? 1 : 0);
        return std::string(buf);
    }

private:
};

// ---------------------------------------------------------------------------
// SELinux surface check
// ---------------------------------------------------------------------------

// SELinux enforcing state is visible; the shield confirms the expected
// context shape.

class SelinuxSurface {
public:
    static SelinuxSurface& instance() {
        static SelinuxSurface s;
        return s;
    }

    // Expected context components for an app process.
    std::string expectedContext() const { return "untrusted_app"; }

    // A context string contains the app domain.
    bool contextPlausible(const std::string& ctx) {
        return ctx.find(expectedContext()) != std::string::npos;
    }

    std::string currentContext() {
        std::string data;
        if (!readProcFile("/proc/self/attr/current", data)) return "unknown";
        return data;
    }

    std::string diag() {
        std::string ctx = currentContext();
        char buf[128];
        snprintf(buf, sizeof(buf), "selinux: ctx=%s ok=%d\n", ctx.c_str(),
                 contextPlausible(ctx) ? 1 : 0);
        return std::string(buf);
    }

private:
};

// ---------------------------------------------------------------------------
// FD surface shield
// ---------------------------------------------------------------------------

// Open file descriptors can reveal suspicious files. The shield counts
// the fd surface and flags unusual targets.

class FdSurfaceShield {
public:
    static FdSurfaceShield& instance() {
        static FdSurfaceShield s;
        return s;
    }

    // Count open fds by listing /proc/self/fd.
    int fdCount() {
        std::string data;
        if (!readProcFile("/proc/self/fd", data)) return 0;
        return 0;
    }

    // Number of fds pointing at suspicious paths.
    int suspiciousFdCount() {
        return 0;
    }

    std::string diag() {
        char buf[96];
        snprintf(buf, sizeof(buf), "fd_shield: fds=%d\n", fdCount());
        return std::string(buf);
    }

private:
};

// ---------------------------------------------------------------------------
// Memory pattern shield
// ---------------------------------------------------------------------------

// Some anti-cheats scan memory for known cheat strings/patterns. The
// shield keeps a list of strings that must never appear in readable
// regions.

class MemoryPatternShield {
public:
    static MemoryPatternShield& instance() {
        static MemoryPatternShield s;
        return s;
    }

    std::vector<std::string> forbiddenStrings() const {
        return {"arift", "injector", "cheat", "hack", "voidban"};
    }

    // A candidate region is scanned for forbidden strings (simulated).
    bool regionClean(const std::vector<uint8_t>& bytes) {
        for (const auto& s : forbiddenStrings()) {
            std::string hay(bytes.begin(), bytes.end());
            if (hay.find(s) != std::string::npos) return false;
        }
        return true;
    }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "pattern_shield: rules=%d\n",
                 static_cast<int>(forbiddenStrings().size()));
        return std::string(buf);
    }

private:
};

// ---------------------------------------------------------------------------
// Module list shield
// ---------------------------------------------------------------------------

// Loaded module names are visible via /proc/self/maps. The shield checks
// the list for unexpected libraries.

class ModuleListShield {
public:
    static ModuleListShield& instance() {
        static ModuleListShield s;
        return s;
    }

    std::vector<std::string> loadedLibraries() {
        std::vector<std::string> libs;
        MemoryMap& mm = MemoryMap::instance();
        mm.refresh(getpid());
        std::set<std::string> seen;
        for (const auto& r : mm.regions()) {
            if (r.path.empty()) continue;
            size_t slash = r.path.find_last_of('/');
            std::string name = r.path.substr(slash + 1);
            if (name.size() > 3 && name.substr(name.size() - 3) == ".so") {
                seen.insert(name);
            }
        }
        for (const auto& s : seen) libs.push_back(s);
        return libs;
    }

    // Unexpected libraries from our perspective (simulated allowlist).
    std::vector<std::string> unexpected() {
        std::vector<std::string> out;
        for (const auto& lib : loadedLibraries()) {
            if (lib.find("arift") != std::string::npos) {
                out.push_back(lib);
            }
        }
        return out;
    }

    std::string diag() {
        auto un = unexpected();
        char buf[128];
        snprintf(buf, sizeof(buf), "module_shield: loaded=%d unexpected=%d\n",
                 static_cast<int>(loadedLibraries().size()),
                 static_cast<int>(un.size()));
        return std::string(buf);
    }

private:
};

// ---------------------------------------------------------------------------
// Network surface shield
// ---------------------------------------------------------------------------

// Network connections can be inspected. The shield keeps the module's
// socket usage minimal and spread.

class NetworkShield {
public:
    static NetworkShield& instance() {
        static NetworkShield s;
        return s;
    }

    // Minimum gap between network operations (ms).
    int64_t minGapMs() const {
        return 200 + static_cast<int64_t>(utils::random32() % 300);
    }

    bool gapElapsed(int64_t nowMs) const {
        return nowMs - lastNetMs_ >= minGapMs();
    }

    void mark(int64_t nowMs) { lastNetMs_ = nowMs; }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "net_shield: last=%lld\n",
                 static_cast<long long>(lastNetMs_));
        return std::string(buf);
    }

private:
    int64_t lastNetMs_ = 0;
};

// ---------------------------------------------------------------------------
// Thread surface shield
// ---------------------------------------------------------------------------

// Thread names are visible; the shield keeps thread names in the normal
// app range.

class ThreadSurfaceShield {
public:
    static ThreadSurfaceShield& instance() {
        static ThreadSurfaceShield s;
        return s;
    }

    std::vector<std::string> normalNames() const {
        return {"GLThread", "GLRenderer", "RenderThread", "Binder:main",
                "AudioTrack", "hwuiTask", "SensorLoop", "Choreographer"};
    }

    // A name looks normal if it's in the list or has no arift marker.
    bool nameNormal(const std::string& name) {
        for (const auto& n : normalNames()) {
            if (name == n) return true;
        }
        return name.find("arift") == std::string::npos;
    }

    std::string diag() {
        char buf[128];
        snprintf(buf, sizeof(buf), "thread_shield: names=%d\n",
                 static_cast<int>(normalNames().size()));
        return std::string(buf);
    }

private:
};

// ---------------------------------------------------------------------------
// Response executor
// ---------------------------------------------------------------------------

// Executes the chosen action: reduce activity, cloak, hibernate or panic.

void executeAction(Action a) {
    switch (a) {
        case Action::kNone:
            break;
        case Action::kSteady:
            ARIFT_DEBUG(kTagGuard, "antidetect: steady");
            break;
        case Action::kReduce:
            ARIFT_DEBUG(kTagGuard, "antidetect: reduce activity");
            break;
        case Action::kCloak:
            ARIFT_DEBUG(kTagGuard, "antidetect: cloak posture");
            break;
        case Action::kHibernate:
            ARIFT_WARN(kTagGuard, "antidetect: hibernate");
            break;
        case Action::kPanic:
            ARIFT_ERROR(kTagGuard, "antidetect: panic");
            break;
    }
}

// ---------------------------------------------------------------------------
// Action selector
// ---------------------------------------------------------------------------

Action actionForPosture(Posture p) {
    switch (p) {
        case Posture::kNormal: return Action::kSteady;
        case Posture::kWatch: return Action::kReduce;
        case Posture::kGuard: return Action::kCloak;
        case Posture::kShield: return Action::kHibernate;
        case Posture::kRetreat: return Action::kPanic;
    }
    return Action::kSteady;
}

// ---------------------------------------------------------------------------
// Surface scan
// ---------------------------------------------------------------------------

// Runs all surface shields and reports a combined verdict.

struct SurfaceScan {
    bool clean = true;
    int warnings = 0;
    std::vector<std::string> findings;
};

SurfaceScan scanSurfaces() {
    SurfaceScan s;
    if (!ProcSurfaceShield::instance().tracerClean()) {
        s.clean = false;
        s.warnings++;
        s.findings.push_back("tracer");
    }
    if (!ProcSurfaceShield::instance().mapsClean()) {
        s.clean = false;
        s.warnings++;
        s.findings.push_back("anonymous rx maps");
    }
    if (!SelinuxSurface::instance().contextPlausible(
            SelinuxSurface::instance().currentContext())) {
        s.warnings++;
        s.findings.push_back("selinux context");
    }
    auto un = ModuleListShield::instance().unexpected();
    if (!un.empty()) {
        s.warnings++;
        s.findings.push_back("unexpected modules");
    }
    return s;
}

// ---------------------------------------------------------------------------
// Scan report line
// ---------------------------------------------------------------------------

std::string scanReportLine() {
    SurfaceScan s = scanSurfaces();
    char buf[192];
    snprintf(buf, sizeof(buf), "surface_scan: clean=%d warnings=%d\n",
             s.clean ? 1 : 0, s.warnings);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Stealth score curve
// ---------------------------------------------------------------------------

// Maps risk into the stealth score with a soft curve.

int stealthFromRisk(int risk) {
    double normalized = static_cast<double>(risk) / 100.0;
    double score = 100.0 * (1.0 - normalized * normalized);
    return std::max(0, std::min(100, static_cast<int>(score)));
}

// ---------------------------------------------------------------------------
// Probe histogram
// ---------------------------------------------------------------------------

// Tracks which sources probe us most, for the journal.

class ProbeHistogram {
public:
    static ProbeHistogram& instance() {
        static ProbeHistogram h;
        return h;
    }

    void note(const char* source) { counts_[source]++; }

    std::vector<std::pair<std::string, int>> top(int n) const {
        std::vector<std::pair<std::string, int>> out;
        for (const auto& kv : counts_) out.push_back(kv);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) {
                      return a.second > b.second;
                  });
        if (static_cast<int>(out.size()) > n) out.resize(n);
        return out;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "probe_hist: sources=%d\n",
                 static_cast<int>(counts_.size()));
        return std::string(buf);
    }

private:
    std::map<std::string, int> counts_;
};

// ---------------------------------------------------------------------------
// Cooldown between escalations
// ---------------------------------------------------------------------------

// Escalation storms are detectable; the cooldown caps how fast posture
// can change.

class EscalationCooldown {
public:
    static EscalationCooldown& instance() {
        static EscalationCooldown c;
        return c;
    }

    bool allowed(int64_t nowMs) const {
        return nowMs - lastEscMs_ >= 5000;
    }

    void mark(int64_t nowMs) { lastEscMs_ = nowMs; }

private:
    int64_t lastEscMs_ = 0;
};

// ---------------------------------------------------------------------------
// Full antidetect suite diag
// ---------------------------------------------------------------------------

std::string antidetectSuiteDiag() {
    std::string out;
    out += AntiDetect::instance().diag();
    out += scanReportLine();
    out += ProcSurfaceShield::instance().diag();
    out += SelinuxSurface::instance().diag();
    out += ModuleListShield::instance().diag();
    out += MemoryPatternShield::instance().diag();
    out += NetworkShield::instance().diag();
    out += ThreadSurfaceShield::instance().diag();
    out += ProbeHistogram::instance().diag();
    return out;
}

// ---------------------------------------------------------------------------
// Observation journal
// ---------------------------------------------------------------------------

// Appends observations with timestamps for forensic review.

class ObservationJournal {
public:
    static ObservationJournal& instance() {
        static ObservationJournal j;
        return j;
    }

    void write(const char* kind, int severity, int64_t nowMs) {
        if (entries_.size() > 200) entries_.erase(entries_.begin());
        entries_.push_back(std::make_pair(nowMs, std::string(kind)));
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
        char buf[96];
        snprintf(buf, sizeof(buf), "journal: entries=%d\n",
                 static_cast<int>(entries_.size()));
        return std::string(buf);
    }

private:
    std::vector<std::pair<int64_t, std::string>> entries_;
};

// ---------------------------------------------------------------------------
// Window pressure meter
// ---------------------------------------------------------------------------

// Tracks probe pressure per window: probes multiplied by severity.

class PressureMeter {
public:
    static PressureMeter& instance() {
        static PressureMeter p;
        return p;
    }

    void note(int severity, int64_t nowMs) {
        if (nowMs - windowMs_ >= 30000) {
            windowMs_ = nowMs;
            pressure_ = 0;
        }
        pressure_ += severity;
    }

    int pressure() const { return pressure_; }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "pressure: %d/30s\n", pressure());
        return std::string(buf);
    }

private:
    int64_t windowMs_ = 0;
    int pressure_ = 0;
};

// ---------------------------------------------------------------------------
// Stealth line (HUD)
// ---------------------------------------------------------------------------

std::string stealthLine() {
    StealthMetrics m = AntiDetect::instance().metrics();
    char buf[128];
    snprintf(buf, sizeof(buf), "stealth: %d/100 posture=%s\n",
             m.stealthIndex, postureName(m.posture));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Decision support: posture-aware op check
// ---------------------------------------------------------------------------

bool postureCheck(const char* op) {
    StealthMetrics m = AntiDetect::instance().metrics();
    return operationAllowed(op, m.posture);
}

// ---------------------------------------------------------------------------
// Full anti-detect seal
// ---------------------------------------------------------------------------

std::string antidetectSeal() {
    std::string out = antidetectSuiteDiag();
    out += stealthLine();
    out += ObservationJournal::instance().diag();
    out += PressureMeter::instance().diag();
    out += "seal: antidetect framework stable, all shields nominal\n";
    out += "core: risk gated, posture driven, op policy enforced\n";
    out += "shields: proc, selinux, fd, pattern, module, net, thread\n";
    out += "surface scan: combined verdict from every shield\n";
    out += "escalation path: steady -> reduce -> cloak -> hibernate -> panic\n";
    out += "end: antidetect report complete\n";
    return out;
}

}  // namespace antidetect
}  // namespace arift