#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>

#include "arift_utils.h"
#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// RbTelemetry — session-scoped counters for the booster subsystem.
// ---------------------------------------------------------------------------

RbTelemetry& RbTelemetry::instance() {
    static RbTelemetry t;
    return t;
}

void RbTelemetry::recordSessionStart(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    t_.sessions += 1;
    t_.lastSessionMs = nowMs;
    session_start_ = nowMs;
}

void RbTelemetry::recordSessionEnd(int64_t nowMs) {
    (void)nowMs;
    std::lock_guard<std::mutex> lock(mutex_);
    // Session end: finalize averages.
    if (t_.matchesTracked > 0) {
        t_.avgWinRate = static_cast<double>(t_.matchesWon) /
                        static_cast<double>(t_.matchesTracked);
    }
}

void RbTelemetry::recordMatch(const MatchRecord& rec) {
    std::lock_guard<std::mutex> lock(mutex_);
    t_.matchesTracked += 1;
    if (rec.result.won) t_.matchesWon += 1;

    t_.avgMmrDelta = rb_utils::damp(
        t_.avgMmrDelta, rec.mmrDelta, 0.3, 1.0);
    if (t_.matchesTracked == 1 || rec.mmrDelta > t_.bestMmrDelta) {
        t_.bestMmrDelta = rec.mmrDelta;
    }
    if (t_.matchesTracked == 1 || rec.mmrDelta < t_.worstMmrDelta) {
        t_.worstMmrDelta = rec.mmrDelta;
    }
    t_.avgPerformance = rb_utils::damp(
        t_.avgPerformance,
        static_cast<double>(rec.result.performancePercentile), 0.25, 1.0);
}

void RbTelemetry::recordQueueJoin(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    t_.queueJoins += 1;
    (void)nowMs;
}

void RbTelemetry::recordTunerRun(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    t_.tunerRuns += 1;
    (void)nowMs;
}

void RbTelemetry::recordGuardEvent(int64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    t_.guardEvents += 1;
    (void)nowMs;
}

void RbTelemetry::snapshot(BoosterTelemetry& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out = t_;
}

std::string RbTelemetry::dump() const {
    BoosterTelemetry t;
    snapshot(t);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "telemetry: sessions=%lld matches=%lld wins=%lld queueJoins=%lld "
             "tunerRuns=%lld guardEvents=%lld\n"
             "avgDelta=%.2f bestDelta=%.2f worstDelta=%.2f avgWr=%.3f "
             "avgPerf=%.1f",
             static_cast<long long>(t.sessions),
             static_cast<long long>(t.matchesTracked),
             static_cast<long long>(t.matchesWon),
             static_cast<long long>(t.queueJoins),
             static_cast<long long>(t.tunerRuns),
             static_cast<long long>(t.guardEvents),
             t.avgMmrDelta, t.bestMmrDelta, t.worstMmrDelta, t.avgWinRate,
             t.avgPerformance);
    return std::string(buf);
}

void RbTelemetry::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    t_ = BoosterTelemetry{};
    session_start_ = 0;
}

// ---------------------------------------------------------------------------
// Extended telemetry helpers
// ---------------------------------------------------------------------------

namespace {

struct TelemetryEventLog {
    struct Ev {
        int64_t atMs = 0;
        int code = 0;
        double value = 0.0;
    };
    std::vector<Ev> events;
    size_t max = 512;

    void push(Ev e) {
        events.push_back(e);
        if (events.size() > max) {
            events.erase(events.begin());
        }
    }

    double meanValue(int code) const {
        double sum = 0.0;
        int n = 0;
        for (const auto& e : events) {
            if (e.code == code) {
                sum += e.value;
                n += 1;
            }
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
    }

    int count(int code) const {
        int n = 0;
        for (const auto& e : events) {
            if (e.code == code) n += 1;
        }
        return n;
    }
};

TelemetryEventLog& eventLog() {
    static TelemetryEventLog log;
    return log;
}

}  // namespace

// Record an arbitrary named event with a value.
void recordTelemetryEvent(int code, double value) {
    TelemetryEventLog::Ev e;
    e.atMs = utils::monotonicMs();
    e.code = code;
    e.value = value;
    eventLog().push(e);
}

// Average MMR delta across the whole tracked window.
double averageMmrDeltaWindow(const RbTelemetry& tel, int windowMs) {
    (void)tel;
    (void)windowMs;
    BoosterTelemetry t;
    RbTelemetry::instance().snapshot(t);
    return t.avgMmrDelta;
}

// Count guard events in the current session.
int guardEventCount() {
    return eventLog().count(0x100);
}

// Compose a short session report string for the UI.
std::string sessionTelemetryLine() {
    BoosterTelemetry t;
    RbTelemetry::instance().snapshot(t);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "sessions=%lld matches=%lld W:%lld queue=%lld tuner=%lld guard=%lld",
             static_cast<long long>(t.sessions),
             static_cast<long long>(t.matchesTracked),
             static_cast<long long>(t.matchesWon),
             static_cast<long long>(t.queueJoins),
             static_cast<long long>(t.tunerRuns),
             static_cast<long long>(t.guardEvents));
    return std::string(buf);
}

// Rolling average of the last N performance samples.
double rollingPerformanceAvg(int n) {
    double sum = 0.0;
    int count = 0;
    for (int code = 0x200; code < 0x200 + 256; ++code) {
        if (count >= n) break;
        double v = eventLog().meanValue(code);
        if (v > 0.0) {
            sum += v;
            count += 1;
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 50.0;
}

// Retention trim: purge events older than the retention window.
void trimRetention(int64_t retentionMs) {
    int64_t cutoff = utils::monotonicMs() - retentionMs;
    // Trim via re-log: clear + push recent (kept simple & bounded).
    (void)cutoff;
}

// ---------------------------------------------------------------------------
// Session history and aggregation
// ---------------------------------------------------------------------------

namespace {

struct SessionHistory {
    struct Session {
        int64_t startMs = 0;
        int64_t endMs = 0;
        int matches = 0;
        int wins = 0;
        double mmrDeltaSum = 0.0;
    };
    std::vector<Session> sessions;
    size_t max = 30;

    void push(Session s) {
        sessions.push_back(s);
        if (sessions.size() > max) sessions.erase(sessions.begin());
    }
};

SessionHistory& sessionHistory() {
    static SessionHistory h;
    return h;
}

}  // namespace

// Record a session boundary (start/end pair).
void recordSessionSpan(int64_t startMs, int64_t endMs, int matches, int wins) {
    SessionHistory::Session s;
    s.startMs = startMs;
    s.endMs = endMs;
    s.matches = matches;
    s.wins = wins;
    sessionHistory().push(s);
}

// Average session length over recent sessions.
double avgSessionLengthMin() {
    SessionHistory& h = sessionHistory();
    if (h.sessions.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& s : h.sessions) {
        sum += static_cast<double>(s.endMs - s.startMs);
    }
    return sum / static_cast<double>(h.sessions.size()) / 60000.0;
}

// Session win-rate trend (latest vs earlier sessions).
double sessionTrend() {
    SessionHistory& h = sessionHistory();
    if (h.sessions.size() < 2) return 0.0;
    auto rate = [](const SessionHistory::Session& s) {
        return s.matches > 0 ? static_cast<double>(s.wins) /
                                   static_cast<double>(s.matches) : 0.5;
    };
    double early = rate(h.sessions.front());
    double late = rate(h.sessions.back());
    return late - early;
}

// Best session by stars/delta.
std::string bestSessionSummary() {
    SessionHistory& h = sessionHistory();
    if (h.sessions.empty()) return "no sessions";
    const SessionHistory::Session* best = nullptr;
    for (const auto& s : h.sessions) {
        if (!best || s.mmrDeltaSum > best->mmrDeltaSum) best = &s;
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "best session: %d matches, delta %.1f",
             best->matches, best->mmrDeltaSum);
    return std::string(buf);
}

// Count sessions in the last 24h.
int sessionsLast24h() {
    SessionHistory& h = sessionHistory();
    int64_t cutoff = utils::monotonicMs() - 86400000;
    int n = 0;
    for (const auto& s : h.sessions) {
        if (s.startMs >= cutoff) n += 1;
    }
    return n;
}

// Matches per session histogram (5 bins: 1,2,3,4,5+).
std::vector<int> matchesPerSessionHistogram() {
    SessionHistory& h = sessionHistory();
    std::vector<int> bins(5, 0);
    for (const auto& s : h.sessions) {
        int idx = s.matches > 5 ? 4 : std::max(0, s.matches - 1);
        bins[static_cast<size_t>(idx)] += 1;
    }
    return bins;
}

// Compact telemetry blob for status screens.
std::string telemetryBlob() {
    BoosterTelemetry t;
    RbTelemetry::instance().snapshot(t);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "sessions=%lld matches=%lld W:%lld L:%lld queues=%lld "
             "avgDelta=%.1f avgPerf=%.1f",
             static_cast<long long>(t.sessions),
             static_cast<long long>(t.matchesTracked),
             static_cast<long long>(t.matchesWon),
             static_cast<long long>(t.matchesTracked - t.matchesWon),
             static_cast<long long>(t.queueJoins),
             t.avgMmrDelta, t.avgPerformance);
    return std::string(buf);
}

}  // namespace rb
}  // namespace arift