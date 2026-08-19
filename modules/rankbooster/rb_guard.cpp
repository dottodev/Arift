#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// RbGuard — booster-specific anti-detection / behavior anomaly checks.
// ---------------------------------------------------------------------------

void RbGuard::recordEvent(const RiskEvent& ev) {
    events_.push_back(ev);
    if (events_.size() > 64) {
        events_.erase(events_.begin());
    }
    risk_score_ += ev.severity;
    if (risk_score_ > 100) risk_score_ = 100;
    last_event_ms_ = ev.atMs;
}

bool RbGuard::riskLevel(const RbConfig& cfg) const {
    return risk_score_ >= cfg.guardSensitivity * 10;
}

std::string RbGuard::dump() const {
    std::string out;
    out += "guard: risk=" + std::to_string(risk_score_) + " events=" +
           std::to_string(events_.size()) + "\n";
    for (const auto& ev : events_) {
        char buf[128];
        snprintf(buf, sizeof(buf), "  [%d] sev=%d %s\n", ev.code, ev.severity,
                 ev.message.c_str());
        out += buf;
    }
    return out;
}

void RbGuard::reset() {
    events_.clear();
    risk_score_ = 0;
    last_event_ms_ = 0;
}

bool RbGuard::checkMatchPattern(const std::vector<MatchRecord>& history,
                                const RbConfig& cfg) const {
    (void)cfg;
    if (history.size() < 8) return false;
    // Flag: win rate far above realistic band while performance average.
    int wins = 0;
    for (const auto& rec : history) {
        if (rec.result.won) wins += 1;
    }
    double wr = static_cast<double>(wins) / static_cast<double>(history.size());
    if (wr > 0.9 && history.size() >= 10) return true;
    // Flag: absurd MMR deltas every match.
    double acc = 0.0;
    for (const auto& rec : history) acc += std::fabs(rec.mmrDelta);
    double avg = acc / static_cast<double>(history.size());
    if (avg > 40.0) return true;
    return false;
}

bool RbGuard::checkTimingPattern(const std::vector<MatchRecord>& history,
                                 const RbConfig& cfg) const {
    (void)cfg;
    if (history.size() < 6) return false;
    // Flag: matches started within seconds of each other (bot-like).
    int suspicious = 0;
    for (size_t i = 1; i < history.size(); ++i) {
        int64_t gap = history[i].startedAtMs - history[i - 1].endedAtMs;
        if (gap > 0 && gap < 4000) suspicious += 1;
    }
    return suspicious >= 4;
}

bool RbGuard::checkPerformanceOutlier(const PlayerProfile& p,
                                      const RbConfig& cfg) const {
    (void)cfg;
    // Flag: performance index far above normal human band while win rate
    // also elevated (combined signal).
    return p.performanceIndex > 85.0 && p.winRate > 0.75 &&
           p.matchesPlayed >= 10;
}

// ---------------------------------------------------------------------------
// Behavioral heuristics
// ---------------------------------------------------------------------------

// Session cadence analysis: distribution of daily match counts.
struct SessionCadence {
    double avgMatchesPerDay = 0.0;
    double maxMatchesPerDay = 0.0;
    double deviation = 0.0;
    bool erratic = false;
};

SessionCadence analyzeCadence(const std::vector<MatchRecord>& history) {
    SessionCadence out;
    if (history.empty()) return out;

    std::map<int64_t, int> byDay;
    for (const auto& rec : history) {
        int64_t day = rec.endedAtMs / 86400000;
        byDay[day] += 1;
    }
    std::vector<double> counts;
    for (const auto& kv : byDay) counts.push_back(static_cast<double>(kv.second));
    out.avgMatchesPerDay = rb_utils::mean(counts);
    out.maxMatchesPerDay = *std::max_element(counts.begin(), counts.end());
    out.deviation = rb_utils::standardDeviation(counts);
    // Erratic if some days have >5x the average.
    if (out.avgMatchesPerDay > 0.0 &&
        out.maxMatchesPerDay > out.avgMatchesPerDay * 5.0) {
        out.erratic = true;
    }
    return out;
}

// Playtime band analysis: does the account play at consistent hours?
struct PlaytimeAnalysis {
    double consistency = 0.0;
    int peakHour = 0;
    std::vector<int> hourHistogram;
};

PlaytimeAnalysis analyzePlaytime(const std::vector<MatchRecord>& history) {
    PlaytimeAnalysis out;
    out.hourHistogram.assign(24, 0);
    if (history.empty()) return out;

    for (const auto& rec : history) {
        time_t sec = static_cast<time_t>(rec.endedAtMs / 1000);
        struct tm tmv{};
        localtime_r(&sec, &tmv);
        out.hourHistogram[static_cast<size_t>(tmv.tm_hour)] += 1;
    }
    int total = 0;
    int peak = 0;
    for (size_t i = 0; i < out.hourHistogram.size(); ++i) {
        total += out.hourHistogram[i];
        if (out.hourHistogram[i] > out.hourHistogram[static_cast<size_t>(peak)]) {
            peak = static_cast<int>(i);
        }
    }
    out.peakHour = peak;
    if (total == 0) return out;

    // Consistency: fraction of hours with activity vs 24h coverage.
    int active = 0;
    for (int c : out.hourHistogram) {
        if (c > 0) active += 1;
    }
    out.consistency = static_cast<double>(active) / 24.0;
    return out;
}

// Streak clustering risk: repeated extreme streaks in short spans.
bool extremeStreakPattern(const std::vector<MatchRecord>& history,
                          int threshold) {
    int run = 0;
    int streaks = 0;
    for (const auto& rec : history) {
        if (rec.result.won) {
            run += 1;
        } else {
            if (run >= threshold) streaks += 1;
            run = 0;
        }
    }
    if (run >= threshold) streaks += 1;
    return streaks >= 3;
}

// Cooldown recommendation (ms) given last actions.
int64_t recommendedCooldownMs(const RbGuard& guard,
                              const std::vector<MatchRecord>& history) {
    int64_t base = 15000;
    if (history.size() >= 8) {
        SessionCadence cadence = analyzeCadence(history);
        if (cadence.erratic) base += 45000;
    }
    if (guard.currentRisk() > 50) base += 60000;
    return base;
}

// ---------------------------------------------------------------------------
// Extended behavioral checks
// ---------------------------------------------------------------------------

// Score current behavioral risk on 0..100 using all signals.
int overallRiskScore(const PlayerProfile& p,
                     const std::vector<MatchRecord>& history,
                     const RbConfig& cfg) {
    int score = 0;
    if (p.winRate > 0.85 && p.matchesPlayed >= 20) score += 25;
    if (p.performanceIndex > 90.0) score += 15;
    if (p.streak >= 8) score += 10;
    if (p.streak <= -8) score += 5;

    SessionCadence cadence = analyzeCadence(history);
    if (cadence.erratic) score += 15;

    double avgDelta = 0.0;
    int n = 0;
    for (const auto& rec : history) {
        avgDelta += std::fabs(rec.mmrDelta);
        n += 1;
    }
    if (n > 0) {
        avgDelta /= static_cast<double>(n);
        if (avgDelta > 30.0) score += 20;
    }

    if (extremeStreakPattern(history, 6)) score += 10;
    if (score > 100) score = 100;
    (void)cfg;
    return score;
}

// Session-pacing check: matches too evenly spaced (bot cadence).
bool evenPacingPattern(const std::vector<MatchRecord>& history,
                       double toleranceRatio) {
    if (history.size() < 8) return false;
    std::vector<int64_t> gaps;
    for (size_t i = 1; i < history.size(); ++i) {
        gaps.push_back(history[i].startedAtMs - history[i - 1].endedAtMs);
    }
    double meanGap = 0.0;
    for (int64_t g : gaps) meanGap += static_cast<double>(g);
    meanGap /= static_cast<double>(gaps.size());
    if (meanGap <= 0.0) return false;
    double maxDev = 0.0;
    for (int64_t g : gaps) {
        double dev = std::fabs(static_cast<double>(g) - meanGap) / meanGap;
        if (dev > maxDev) maxDev = dev;
    }
    return maxDev < toleranceRatio;
}

// Hourly-consistency check: activity confined to a narrow hour window.
bool narrowWindowPattern(const std::vector<MatchRecord>& history) {
    PlaytimeAnalysis pt = analyzePlaytime(history);
    if (pt.hourHistogram.empty()) return false;
    int active = 0;
    for (int c : pt.hourHistogram) {
        if (c > 0) active += 1;
    }
    return history.size() >= 15 && active <= 3;
}

// MMR monotonicity check: continuous positive drift with no dips.
bool monotonicDriftPattern(const std::vector<MatchRecord>& history,
                           int minSamples) {
    if (history.size() < static_cast<size_t>(minSamples)) return false;
    int positives = 0;
    for (const auto& rec : history) {
        if (rec.mmrDelta > 0.0) positives += 1;
    }
    return static_cast<double>(positives) /
               static_cast<double>(history.size()) > 0.95;
}

// Aggregate guard verdict with per-check breakdown.
struct GuardVerdict {
    int risk = 0;
    int checksTriggered = 0;
    std::vector<std::string> triggers;
    bool safe = true;
};

GuardVerdict assessVerdict(const PlayerProfile& p,
                           const std::vector<MatchRecord>& history,
                           const RbConfig& cfg) {
    GuardVerdict v;
    v.risk = overallRiskScore(p, history, cfg);

    if (evenPacingPattern(history, 0.08)) {
        v.triggers.push_back("even pacing");
        v.checksTriggered += 1;
    }
    if (narrowWindowPattern(history)) {
        v.triggers.push_back("narrow window");
        v.checksTriggered += 1;
    }
    if (monotonicDriftPattern(history, 12)) {
        v.triggers.push_back("monotonic drift");
        v.checksTriggered += 1;
    }
    if (extremeStreakPattern(history, 7)) {
        v.triggers.push_back("extreme streaks");
        v.checksTriggered += 1;
    }

    int threshold = cfg.guardSensitivity * 10;
    v.safe = v.risk < threshold && v.checksTriggered < 2;
    return v;
}

// Suggest a pause duration proportional to risk.
int64_t pauseDurationForRisk(int risk) {
    int64_t base = 60000;
    if (risk >= 80) return base * 5;
    if (risk >= 60) return base * 3;
    if (risk >= 40) return base * 2;
    return base;
}

// ---------------------------------------------------------------------------
// Risk models
// ---------------------------------------------------------------------------

namespace {

// Logistic mapping of a raw score into 0..1 probability.
double logisticRisk(double raw) {
    return 1.0 / (1.0 + std::exp(-(raw - 50.0) / 15.0));
}

}  // namespace

// Risk probability from the raw score (calibrated curve).
double riskProbability(int rawScore) {
    return logisticRisk(static_cast<double>(rawScore));
}

// Composite signal from all behavioral checks (0..100).
double compositeSignal(const PlayerProfile& p,
                       const std::vector<MatchRecord>& history) {
    double signal = 0.0;
    signal += rb_utils::clamp01((p.winRate - 0.5) * 2.0) * 40.0;
    signal += rb_utils::clamp01((p.performanceIndex - 50.0) / 50.0) * 30.0;
    signal += rb_utils::clamp01(static_cast<double>(std::abs(p.streak)) / 10.0) * 20.0;
    signal += rb_utils::clamp01(p.volatility / 0.12) * 10.0;
    return rb_utils::clamp(signal, 0.0, 100.0);
}

// Decay risk score over time (risk cools down between sessions).
int decayedRisk(int rawScore, int64_t elapsedMs, double halfLifeMs) {
    if (rawScore <= 0) return 0;
    double factor = std::pow(0.5, static_cast<double>(elapsedMs) / halfLifeMs);
    return std::max(0, static_cast<int>(static_cast<double>(rawScore) * factor));
}

// Session boundary check: long gaps between matches reduce risk.
double gapDiscount(int64_t gapMs) {
    if (gapMs <= 0) return 1.0;
    if (gapMs >= 3600000) return 0.3;
    if (gapMs >= 900000) return 0.6;
    return 0.85;
}

// Streak risk profile: which streak lengths are suspicious.
int streakRisk(int streak) {
    int s = std::abs(streak);
    if (s >= 10) return 30;
    if (s >= 7) return 20;
    if (s >= 5) return 10;
    return 0;
}

// Performance-band risk: consistent 90+ percentile play.
int performanceBandRisk(double avgPerformance) {
    if (avgPerformance >= 95.0) return 25;
    if (avgPerformance >= 88.0) return 15;
    if (avgPerformance >= 80.0) return 8;
    return 0;
}

// KDA anomaly risk: absurd KDA ratios sustained over time.
int kdaAnomalyRisk(const std::vector<MatchRecord>& history) {
    if (history.size() < 10) return 0;
    double total = 0.0;
    int n = 0;
    for (const auto& rec : history) {
        if (rec.result.kdaDeaths > 0) {
            total += static_cast<double>(rec.result.kdaKills) /
                     static_cast<double>(rec.result.kdaDeaths);
            n += 1;
        }
    }
    if (n < 8) return 0;
    double avg = total / static_cast<double>(n);
    if (avg >= 8.0) return 20;
    if (avg >= 5.0) return 10;
    return 0;
}

// Combined risk report with contributions.
struct RiskReport {
    int total = 0;
    int streakRisk = 0;
    int perfRisk = 0;
    int kdaRisk = 0;
    int cadenceRisk = 0;
    std::string summary;
};

RiskReport buildRiskReport(const PlayerProfile& p,
                           const std::vector<MatchRecord>& history) {
    RiskReport r;
    r.streakRisk = streakRisk(static_cast<int>(p.streak));
    r.perfRisk = performanceBandRisk(p.performanceIndex);
    r.kdaRisk = kdaAnomalyRisk(history);
    SessionCadence cadence = analyzeCadence(history);
    r.cadenceRisk = cadence.erratic ? 15 : 0;
    r.total = std::min(100, r.streakRisk + r.perfRisk + r.kdaRisk +
                                r.cadenceRisk);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "total=%d (streak=%d perf=%d kda=%d cadence=%d)",
             r.total, r.streakRisk, r.perfRisk, r.kdaRisk, r.cadenceRisk);
    r.summary = buf;
    return r;
}

// Safety margin: how close we are to the risky threshold.
double safetyMargin(int risk, int threshold) {
    if (threshold <= 0) return 1.0;
    return rb_utils::clamp01(1.0 - static_cast<double>(risk) /
                                       static_cast<double>(threshold));
}

// ---------------------------------------------------------------------------
// Guard policies
// ---------------------------------------------------------------------------

// Risk band label for the UI.
const char* riskBandLabel(int risk) {
    if (risk >= 75) return "CRITICAL";
    if (risk >= 50) return "HIGH";
    if (risk >= 25) return "MODERATE";
    if (risk > 0) return "LOW";
    return "CLEAR";
}

// Max consecutive matches before a forced pause.
int maxConsecutiveMatches(int guardRisk) {
    if (guardRisk >= 60) return 2;
    if (guardRisk >= 30) return 4;
    return 6;
}

// Suggested wait after a guard pause (minutes).
double guardPauseMinutes(int risk, int sensitivity) {
    double base = 5.0 + static_cast<double>(sensitivity) * 3.0;
    if (risk >= 75) base *= 3.0;
    else if (risk >= 50) base *= 2.0;
    return base;
}

// Verify a single match record for anomalies.
std::vector<std::string> verifyMatch(const MatchRecord& rec) {
    std::vector<std::string> issues;
    if (rec.result.durationMin < 5.0 || rec.result.durationMin > 60.0) {
        issues.push_back("duration out of range");
    }
    if (rec.result.kdaKills < 0 || rec.result.kdaKills > 50) {
        issues.push_back("kills out of range");
    }
    if (rec.result.performancePercentile < 0 ||
        rec.result.performancePercentile > 100) {
        issues.push_back("performance out of range");
    }
    if (rec.endedAtMs < rec.startedAtMs) {
        issues.push_back("timestamps inverted");
    }
    if (std::fabs(rec.mmrDelta) > 100.0) {
        issues.push_back("mmr delta implausible");
    }
    return issues;
}

// History integrity score (fraction of records without issues).
double historyIntegrity(const std::vector<MatchRecord>& history) {
    if (history.empty()) return 1.0;
    int bad = 0;
    for (const auto& rec : history) {
        if (!verifyMatch(rec).empty()) bad += 1;
    }
    return 1.0 - static_cast<double>(bad) / static_cast<double>(history.size());
}

// Anomaly rate per 100 matches (for trend monitoring).
double anomalyRatePer100(const std::vector<MatchRecord>& history) {
    if (history.empty()) return 0.0;
    int bad = 0;
    for (const auto& rec : history) {
        if (!verifyMatch(rec).empty()) bad += 1;
    }
    return static_cast<double>(bad) * 100.0 /
           static_cast<double>(history.size());
}

// Guard trigger history fingerprint (opaque, for cache).
std::string guardFingerprint(const RbGuard& guard) {
    std::string fp = "g" + std::to_string(guard.currentRisk()) + "e" +
                     std::to_string(guard.lastEventMs());
    return fp;
}

}  // namespace rb
}  // namespace arift